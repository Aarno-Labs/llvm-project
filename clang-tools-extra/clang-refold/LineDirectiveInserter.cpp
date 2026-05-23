#include "LineDirectiveInserter.h"
#include "RefoldLog.h"
#include "StringUtils.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

// Minimal macro record used only while reconstructing source-authored
// line-control directives.  This is intentionally not a general macro model:
// it is the small deterministic subset needed to answer one question during
// resync emission: "what logical file/line would the preprocessor assign at
// this owner-local byte offset after macro-expanding a #line directive?"
struct LineControlMacroDefinition {
  bool functionLike = false;
  std::vector<std::string> params;

  // The local evaluator needs to recognize both standard C variadics
  // (`...` / `__VA_ARGS__`) and GNU named variadics (`args...`) because
  // either spelling may provide the filename operand of a source-authored
  // `#line` directive.  The variadic parameter is still stored in `params`
  // at its normal position; these fields only identify that parameter so call
  // arguments can be coalesced the same way the preprocessor forms the
  // variadic argument.
  bool variadic = false;
  std::string variadicParam;

  std::string replacement;
};

using LineControlMacroMap =
    std::unordered_map<std::string, LineControlMacroDefinition>;

static StringRef trimHorizontal(StringRef text) {
  size_t begin = 0;
  while (begin < text.size() && stringutils::isWs(text[begin]) &&
         text[begin] != '\n')
    ++begin;

  size_t end = text.size();
  while (end > begin && stringutils::isWs(text[end - 1]) &&
         text[end - 1] != '\n')
    --end;
  return text.slice(begin, end);
}

// Implement the whitespace normalization required by macro stringification.
// `# x` in a replacement list stringifies the *raw* argument after trimming
// leading/trailing horizontal whitespace and collapsing internal whitespace
// runs to one space.  The line-control evaluator needs this for constructs like
// `#line 620 STR(logical_file.c)`.
static std::string collapseWhitespaceForStringification(StringRef text) {
  std::string out;
  bool inWs = false;
  StringRef trimmed = trimHorizontal(text);
  for (size_t i = 0; i < trimmed.size(); ++i) {
    char c = trimmed[i];
    if (stringutils::isWs(c)) {
      inWs = true;
      continue;
    }
    if (inWs && !out.empty())
      out.push_back(' ');
    inWs = false;
    out.push_back(c);
  }
  return out;
}

// Spell the result of stringifying one raw macro argument as a C string literal.
// The later #line filename parser decodes that literal, so this routine should
// preserve the preprocessor spelling contract rather than the decoded filename.
static std::string spellStringifiedMacroArgument(StringRef arg) {
  std::string normalized = collapseWhitespaceForStringification(arg);
  std::string out;
  out.reserve(normalized.size() + 2);
  out.push_back('"');
  for (char c : normalized) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static std::string basenameForLineControlFile(StringRef file) {
  size_t slash = file.find_last_of("/\\");
  if (slash == StringRef::npos)
    return file.str();
  return file.substr(slash + 1).str();
}

static std::string quoteLineControlFile(StringRef file) {
  std::string out;
  out.reserve(file.size() + 2);
  out.push_back('"');
  out += LineDirectiveInserter::EscapeForLineDirective(file);
  out.push_back('"');
  return out;
}

// Copy a string/character literal without interpreting identifiers inside it.
// Macro replacement does not occur inside quoted literals, and preserving the
// raw literal spelling is important for #line filename decoding.
static bool copyQuotedLiteral(StringRef text, size_t &i, std::string &out) {
  if (i >= text.size() || (text[i] != '"' && text[i] != '\''))
    return false;

  const char quote = text[i];
  out.push_back(text[i++]);
  while (i < text.size()) {
    char c = text[i++];
    out.push_back(c);
    if (c == '\\' && i < text.size()) {
      out.push_back(text[i++]);
      continue;
    }
    if (c == quote)
      break;
  }
  return true;
}

static bool skipQuotedLiteral(StringRef text, size_t &i) {
  std::string ignored;
  return copyQuotedLiteral(text, i, ignored);
}

// Parse a function-like macro invocation argument list well enough for #line
// operands.  Arguments may contain nested parentheses and quoted literals; the
// result is raw, trimmed argument spelling because stringification must see raw
// arguments, while ordinary substitution separately uses expanded arguments.
static std::optional<std::vector<std::string>> parseMacroArguments(
    StringRef text, size_t openParen, size_t &afterClose) {
  if (openParen >= text.size() || text[openParen] != '(')
    return std::nullopt;

  std::vector<std::string> args;
  std::string cur;
  unsigned depth = 0;

  for (size_t i = openParen + 1; i < text.size(); ++i) {
    char c = text[i];

    if (c == '"' || c == '\'') {
      size_t literalBegin = i;
      if (!skipQuotedLiteral(text, i))
        return std::nullopt;
      cur += text.slice(literalBegin, i).str();
      --i;
      continue;
    }

    if (c == '(') {
      ++depth;
      cur.push_back(c);
      continue;
    }

    if (c == ')') {
      if (depth == 0) {
        if (!args.empty() || !trimHorizontal(StringRef(cur)).empty())
          args.push_back(trimHorizontal(StringRef(cur)).str());
        afterClose = i + 1;
        return args;
      }
      --depth;
      cur.push_back(c);
      continue;
    }

    if (c == ',' && depth == 0) {
      args.push_back(trimHorizontal(StringRef(cur)).str());
      cur.clear();
      continue;
    }

    cur.push_back(c);
  }

  return std::nullopt;
}


// Parse one balanced parenthesized token sequence without treating top-level
// commas as separators.  `__VA_OPT__(...)` takes a single token sequence, and
// that sequence may itself contain commas, nested parentheses, or quoted
// literals.  This helper returns the raw spelling inside the outer parentheses
// so the ordinary replacement-list substitution code can evaluate the content
// if the variadic argument is present.
static std::optional<std::string> parseBalancedParenthesizedContent(
    StringRef text, size_t openParen, size_t &afterClose) {
  if (openParen >= text.size() || text[openParen] != '(')
    return std::nullopt;

  std::string content;
  unsigned depth = 0;

  for (size_t i = openParen + 1; i < text.size(); ++i) {
    char c = text[i];

    if (c == '"' || c == '\'') {
      size_t literalBegin = i;
      if (!skipQuotedLiteral(text, i))
        return std::nullopt;
      content += text.slice(literalBegin, i).str();
      --i;
      continue;
    }

    if (c == '(') {
      ++depth;
      content.push_back(c);
      continue;
    }

    if (c == ')') {
      if (depth == 0) {
        afterClose = i + 1;
        return content;
      }
      --depth;
      content.push_back(c);
      continue;
    }

    content.push_back(c);
  }

  return std::nullopt;
}

static std::string joinRawMacroArguments(ArrayRef<std::string> args,
                                         size_t begin) {
  if (begin >= args.size())
    return "";

  std::string out = args[begin];
  for (size_t i = begin + 1; i < args.size(); ++i) {
    // Preserve the comma separators that are part of a variadic argument.  The
    // exact horizontal whitespace around the comma is not significant for the
    // line-control operands we later parse, but using a stable spelling keeps
    // diagnostics and traces deterministic.
    out += ", ";
    out += args[i];
  }
  return out;
}

// The #line evaluator only needs the spelling produced by simple token paste in
// line-control operands.  After parameter substitution, remove `##` and adjacent
// horizontal padding so pasted numeric/string/file-name fragments can be parsed
// by the normal #line parser.  This stays local to line-control recovery and is
// not used for refolding arbitrary macro programs.
static std::string removeTokenPasteOperators(StringRef text) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    if (copyQuotedLiteral(text, i, out))
      continue;

    if (i + 1 < text.size() && text[i] == '#' && text[i + 1] == '#') {
      while (!out.empty() && stringutils::isWs(out.back()) && out.back() != '\n')
        out.pop_back();
      i += 2;
      while (i < text.size() && stringutils::isWs(text[i]) && text[i] != '\n')
        ++i;
      continue;
    }

    out.push_back(text[i++]);
  }

  return out;
}

static std::string expandLineControlMacros(
    StringRef text, const LineControlMacroMap &macros,
    std::unordered_set<std::string> &disabled, size_t logicalLineAtLineStart,
    StringRef activeFileSpelling);

static std::string substituteLineControlReplacementFragment(
    StringRef repl, const std::unordered_map<std::string, std::string> &rawByParam,
    const std::unordered_map<std::string, std::string> &expandedByParam,
    bool variadicArgumentHasTokens, const LineControlMacroMap &macros,
    std::unordered_set<std::string> &disabled, size_t logicalLineAtLineStart,
    StringRef activeFileSpelling);

static bool isHorizontalWhitespace(char c) {
  return stringutils::isWs(c) && c != '\n';
}

// Return true iff the replacement-list token [nameBegin, nameEnd) is an
// operand of a token-paste operator.  Macro arguments adjacent to `##` are not
// macro-expanded before substitution; their raw tokens are substituted, the
// paste is formed, and the pasted token is then rescanned.  This is the critical
// preprocessor invariant for source line-control macros such as:
//
//   #define RAW 4
//   #define RAW00 910
//   #define LOC(x) x ## 00 "f.c"
//   #line LOC(RAW)
//
// The argument token `RAW` must paste into `RAW00` and only then expand to
// `910`; expanding `RAW` first would incorrectly produce line `400`.
static bool replacementTokenIsAdjacentToPaste(StringRef repl, size_t nameBegin,
                                              size_t nameEnd) {
  size_t before = nameBegin;
  while (before > 0 && isHorizontalWhitespace(repl[before - 1]))
    --before;
  if (before >= 2 && repl[before - 2] == '#' && repl[before - 1] == '#')
    return true;

  size_t after = nameEnd;
  while (after < repl.size() && isHorizontalWhitespace(repl[after]))
    ++after;
  return after + 1 < repl.size() && repl[after] == '#' &&
         repl[after + 1] == '#';
}

// Substitute a function-like macro invocation for the line-control evaluator.
// This mirrors the C preprocessor distinction needed by #line operands:
//   * `#param` uses the raw argument spelling;
//   * ordinary `param` uses the macro-expanded argument spelling;
//   * `__VA_OPT__(tokens)` contributes `tokens` only when the variadic
//     argument is non-empty;
//   * `##` is resolved after substitution.
// The caller maintains the disabled set so recursive macro references are left
// unexpanded instead of causing unbounded recursion.
static std::string substituteFunctionLikeLineControlMacro(
    const LineControlMacroDefinition &def, ArrayRef<std::string> rawArgs,
    const LineControlMacroMap &macros, std::unordered_set<std::string> &disabled,
    size_t logicalLineAtLineStart, StringRef activeFileSpelling) {
  std::unordered_map<std::string, std::string> rawByParam;
  std::unordered_map<std::string, std::string> expandedByParam;
  bool variadicArgumentHasTokens = false;

  for (size_t i = 0; i < def.params.size(); ++i) {
    std::string raw;
    if (def.variadic && def.params[i] == def.variadicParam) {
      raw = joinRawMacroArguments(rawArgs, i);
      variadicArgumentHasTokens = !trimHorizontal(StringRef(raw)).empty();
    } else {
      raw = i < rawArgs.size() ? rawArgs[i] : std::string();
    }

    rawByParam[def.params[i]] = raw;
    expandedByParam[def.params[i]] = expandLineControlMacros(
        raw, macros, disabled, logicalLineAtLineStart, activeFileSpelling);
  }

  std::string substituted = substituteLineControlReplacementFragment(
      StringRef(def.replacement), rawByParam, expandedByParam,
      variadicArgumentHasTokens, macros, disabled, logicalLineAtLineStart,
      activeFileSpelling);

  std::string pasted = removeTokenPasteOperators(StringRef(substituted));
  return expandLineControlMacros(StringRef(pasted), macros, disabled,
                                 logicalLineAtLineStart, activeFileSpelling);
}

static std::string substituteLineControlReplacementFragment(
    StringRef repl, const std::unordered_map<std::string, std::string> &rawByParam,
    const std::unordered_map<std::string, std::string> &expandedByParam,
    bool variadicArgumentHasTokens, const LineControlMacroMap &macros,
    std::unordered_set<std::string> &disabled, size_t logicalLineAtLineStart,
    StringRef activeFileSpelling) {
  std::string substituted;
  substituted.reserve(repl.size());

  for (size_t i = 0; i < repl.size();) {
    if (copyQuotedLiteral(repl, i, substituted))
      continue;

    if (repl[i] == '#') {
      if (i + 1 < repl.size() && repl[i + 1] == '#') {
        substituted += "##";
        i += 2;
        continue;
      }

      size_t j = i + 1;
      while (j < repl.size() && stringutils::isWs(repl[j]) && repl[j] != '\n')
        ++j;
      if (j < repl.size() && stringutils::isIdentStart(repl[j])) {
        size_t nameBegin = j++;
        while (j < repl.size() && stringutils::isIdentPart(repl[j]))
          ++j;
        std::string name = repl.slice(nameBegin, j).str();
        auto found = rawByParam.find(name);
        if (found != rawByParam.end()) {
          substituted += spellStringifiedMacroArgument(found->second);
          i = j;
          continue;
        }
      }
    }

    if (stringutils::isIdentStart(repl[i])) {
      size_t nameBegin = i++;
      while (i < repl.size() && stringutils::isIdentPart(repl[i]))
        ++i;
      std::string name = repl.slice(nameBegin, i).str();

      // `__VA_OPT__` is a replacement-list operator, not an ordinary macro.
      // Evaluate it while the raw/expanded parameter bindings are still in
      // scope.  If the variadic argument is empty, the whole parenthesized token
      // sequence disappears; otherwise the token sequence is substituted using
      // the same rules as the surrounding replacement list.  This is the piece
      // needed for line-control forms such as:
      //   #define LOC(n, ...) n __VA_OPT__(__VA_ARGS__)
      //   #define LOC(n, name, ...) n __VA_OPT__(#name)
      if (name == "__VA_OPT__") {
        size_t callPos = i;
        while (callPos < repl.size() && stringutils::isWs(repl[callPos]) &&
               repl[callPos] != '\n')
          ++callPos;
        if (callPos < repl.size() && repl[callPos] == '(') {
          size_t afterClose = callPos;
          std::optional<std::string> content =
              parseBalancedParenthesizedContent(repl, callPos, afterClose);
          if (content) {
            if (variadicArgumentHasTokens) {
              substituted += substituteLineControlReplacementFragment(
                  StringRef(*content), rawByParam, expandedByParam,
                  variadicArgumentHasTokens, macros, disabled,
                  logicalLineAtLineStart, activeFileSpelling);
            }
            i = afterClose;
            continue;
          }
        }

        substituted += name;
        continue;
      }

      // Ordinary parameter substitution uses the macro-expanded argument,
      // except when the parameter is an operand of `##`.  In the paste case the
      // C preprocessor substitutes raw argument tokens, forms the pasted token,
      // and only then rescans that token for further macro expansion.  The
      // final rescan happens after `removeTokenPasteOperators()` below.
      if (replacementTokenIsAdjacentToPaste(repl, nameBegin, i)) {
        auto raw = rawByParam.find(name);
        if (raw != rawByParam.end()) {
          substituted += raw->second;
          continue;
        }
      }

      auto found = expandedByParam.find(name);
      if (found != expandedByParam.end()) {
        substituted += found->second;
        continue;
      }
      substituted += name;
      continue;
    }

    substituted.push_back(repl[i++]);
  }

  return substituted;
}

// Expand macro names in the operand portion of a source-authored line-control
// directive.  The expansion context is the logical location at the directive
// line itself, which is what predefined macros such as __LINE__ and __FILE__
// observe when they appear inside `#line` operands.
static std::string expandLineControlMacros(
    StringRef text, const LineControlMacroMap &macros,
    std::unordered_set<std::string> &disabled, size_t logicalLineAtLineStart,
    StringRef activeFileSpelling) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    if (copyQuotedLiteral(text, i, out))
      continue;

    if (!stringutils::isIdentStart(text[i])) {
      out.push_back(text[i++]);
      continue;
    }

    const size_t nameBegin = i++;
    while (i < text.size() && stringutils::isIdentPart(text[i]))
      ++i;

    std::string name = text.slice(nameBegin, i).str();

    // Predefined location macros in #line operands expand using the logical
    // state active at the directive line before this directive takes effect.
    if (name == "__LINE__") {
      out += std::to_string(logicalLineAtLineStart);
      continue;
    }
    if (name == "__FILE__") {
      out += quoteLineControlFile(activeFileSpelling);
      continue;
    }
    if (name == "__FILE_NAME__") {
      out += quoteLineControlFile(basenameForLineControlFile(activeFileSpelling));
      continue;
    }

    auto found = macros.find(name);
    if (found == macros.end() || disabled.count(name)) {
      out += name;
      continue;
    }

    const LineControlMacroDefinition &def = found->second;
    if (!def.functionLike) {
      disabled.insert(name);
      out += expandLineControlMacros(StringRef(def.replacement), macros,
                                     disabled, logicalLineAtLineStart,
                                     activeFileSpelling);
      disabled.erase(name);
      continue;
    }

    // At invocation sites, whitespace may appear between the macro name and
    // the argument list.  This differs from definition-site recognition, where
    // `NAME(` must be immediate to define a function-like macro.
    size_t callPos = i;
    while (callPos < text.size() && stringutils::isWs(text[callPos]) &&
           text[callPos] != '\n')
      ++callPos;
    if (callPos >= text.size() || text[callPos] != '(') {
      out += name;
      continue;
    }

    size_t afterClose = callPos;
    std::optional<std::vector<std::string>> args =
        parseMacroArguments(text, callPos, afterClose);
    if (!args) {
      out += name;
      continue;
    }

    disabled.insert(name);
    out += substituteFunctionLikeLineControlMacro(
        def, ArrayRef<std::string>(*args), macros, disabled,
        logicalLineAtLineStart, activeFileSpelling);
    disabled.erase(name);
    i = afterClose;
  }

  return out;
}

static std::string expandLineControlMacros(
    StringRef text, const LineControlMacroMap &macros,
    size_t logicalLineAtLineStart, StringRef activeFileSpelling) {
  std::unordered_set<std::string> disabled;
  return expandLineControlMacros(text, macros, disabled, logicalLineAtLineStart,
                                 activeFileSpelling);
}

static bool lineStartsWithHash(StringRef line, size_t &p, size_t to) {
  while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
    ++p;
  if (p >= to || line[p] != '#')
    return false;
  ++p;
  while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
    ++p;
  return true;
}

static bool readDirectiveIdentifier(StringRef line, size_t &p, size_t to,
                                    StringRef &ident) {
  if (p >= to || !stringutils::isIdentStart(line[p]))
    return false;
  const size_t begin = p;
  ++p;
  while (p < to && stringutils::isIdentPart(line[p]))
    ++p;
  ident = line.slice(begin, p);
  return true;
}

// Update the owner-local macro environment from source directives that precede
// the offset being queried.  Only definitions visible in the same source owner
// are considered; this deliberately avoids importing cross-owner macro state
// into line repair and keeps the proof local to the text being emitted.
static void updateLineControlMacroEnvironment(StringRef line,
                                              LineControlMacroMap &macros) {
  const size_t to = line.size();
  size_t p = 0;
  if (!lineStartsWithHash(line, p, to))
    return;

  StringRef directive;
  if (!readDirectiveIdentifier(line, p, to, directive))
    return;

  if (directive == "undef") {
    while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
      ++p;
    StringRef name;
    if (readDirectiveIdentifier(line, p, to, name))
      macros.erase(name.str());
    return;
  }

  if (directive != "define")
    return;

  if (p >= to || !stringutils::isWs(line[p]) || line[p] == '\n')
    return;
  while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
    ++p;

  StringRef name;
  if (!readDirectiveIdentifier(line, p, to, name))
    return;

  LineControlMacroDefinition def;

  // Function-like macro definitions are recognized only when the left
  // parenthesis immediately follows the macro name.  This is the C
  // preprocessor's definition-site rule; invocations may still have whitespace
  // before their argument list.  The evaluator is intentionally scoped to the
  // source-prefix line-control proof and never changes refold ownership.
  if (p < to && line[p] == '(') {
    def.functionLike = true;
    ++p;
    while (p < to) {
      while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
        ++p;
      if (p < to && line[p] == ')') {
        ++p;
        break;
      }

      if (p + 3 <= to && line.substr(p, 3) == "...") {
        // Standard variadic macro spelling.  Inside the replacement list the
        // variadic argument is addressed as `__VA_ARGS__`.
        def.variadic = true;
        def.variadicParam = "__VA_ARGS__";
        def.params.push_back(def.variadicParam);
        p += 3;
      } else {
        StringRef param;
        if (!readDirectiveIdentifier(line, p, to, param)) {
          macros.erase(name.str());
          return;
        }

        if (p + 3 <= to && line.substr(p, 3) == "...") {
          // GNU named variadic spelling, e.g. `#define LOC(args...) args`.
          // The named parameter receives the complete variadic argument.
          def.variadic = true;
          def.variadicParam = param.str();
          def.params.push_back(def.variadicParam);
          p += 3;
        } else {
          def.params.push_back(param.str());
        }
      }

      while (p < to && stringutils::isWs(line[p]) && line[p] != '\n')
        ++p;
      if (p < to && line[p] == ',') {
        if (def.variadic) {
          macros.erase(name.str());
          return;
        }
        ++p;
        continue;
      }
      if (p < to && line[p] == ')') {
        ++p;
        break;
      }
      macros.erase(name.str());
      return;
    }
  }

  def.replacement = trimHorizontal(line.substr(p)).str();
  macros[name.str()] = std::move(def);
}

// Build the preprocessing-logical form of one source line for source-authored
// line-control recovery.
//
// `LogicalLocationAtOffset()` asks a preprocessor question: "after executing
// all source directives before this byte offset, what logical file/line is
// active?"  That question must be answered after the early translation phases
// that affect preprocessing directives.  In particular, phase 2 removes
// backslash-newline pairs before directive recognition, so both
//
//   #define LOC \
//     930 "f.c"
//
// and
//
//   #line 950 \
//   "f.c"
//
// are single preprocessing directive lines even though they occupy multiple
// physical file lines.  This helper removes only complete splice pairs inside
// the already-bounded owner prefix and returns the raw byte offset immediately
// after the non-spliced newline that terminates the directive.
static bool collectLineControlLogicalLine(StringRef src, size_t lineBegin,
                                          size_t limit,
                                          std::string &logicalLine,
                                          size_t &afterLine) {
  if (lineBegin > limit || limit > src.size())
    return false;

  logicalLine.clear();

  auto skipPhase2Splice = [&](size_t &pos) -> bool {
    if (src[pos] != '\\')
      return false;
    if (pos + 1 < limit && src[pos + 1] == '\n') {
      pos += 2;
      return true;
    }
    if (pos + 2 < limit && src[pos + 1] == '\r' && src[pos + 2] == '\n') {
      pos += 3;
      return true;
    }
    return false;
  };

  for (size_t pos = lineBegin; pos < limit;) {
    if (skipPhase2Splice(pos))
      continue;

    // Comments are not recognized inside string or character literals.  Still
    // apply phase-2 splicing while copying the literal, because splices are
    // removed before the preprocessor even sees the directive spelling.
    if (src[pos] == '"' || src[pos] == '\'') {
      const char quote = src[pos];
      logicalLine.push_back(src[pos++]);
      while (pos < limit) {
        if (skipPhase2Splice(pos))
          continue;
        char c = src[pos++];
        logicalLine.push_back(c);
        if (c == '\\' && pos < limit) {
          if (skipPhase2Splice(pos))
            continue;
          logicalLine.push_back(src[pos++]);
          continue;
        }
        if (c == quote)
          break;
        if (c == '\n')
          return false;
      }
      continue;
    }

    // Do not let a physical newline inside a complete block comment terminate
    // the directive.  Phase 3 replaces the whole comment with one whitespace
    // character before directive macro expansion/parsing.
    if (pos + 1 < limit && src[pos] == '/' && src[pos + 1] == '*') {
      logicalLine.push_back(src[pos++]);
      logicalLine.push_back(src[pos++]);
      bool closed = false;
      while (pos < limit) {
        if (skipPhase2Splice(pos))
          continue;
        if (pos + 1 < limit && src[pos] == '*' && src[pos + 1] == '/') {
          logicalLine.push_back(src[pos++]);
          logicalLine.push_back(src[pos++]);
          closed = true;
          break;
        }
        logicalLine.push_back(src[pos++]);
      }
      if (!closed)
        return false;
      continue;
    }

    if (src[pos] == '\n') {
      afterLine = pos + 1;
      return true;
    }

    logicalLine.push_back(src[pos++]);
  }

  afterLine = limit;
  return true;
}

// Apply translation phase 3 to the collected directive line: comments become a
// single whitespace character.  This is deliberately small and lexical: it is
// only used before #define/#undef bookkeeping and #line operand parsing.  String
// and character literals are copied verbatim because comments are not recognized
// inside them.
static std::string replaceCommentsWithWhitespaceForLineControl(StringRef text) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    if (copyQuotedLiteral(text, i, out))
      continue;

    if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
      out.push_back(' ');
      break;
    }

    if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
      out.push_back(' ');
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/'))
        ++i;
      if (i + 1 < text.size())
        i += 2;
      continue;
    }

    out.push_back(text[i++]);
  }

  return out;
}

// Return the logical line number at the start of a physical source line while
// scanning the prefix.  If no source-authored line-control directive has been
// seen, physical and logical lines coincide.  After a directive, the logical
// line advances from the directive's post-line state by the number of physical
// lines scanned since that directive.
static size_t logicalLineAtSourceOffset(StringRef prefix, size_t lineStart,
                                        bool sawLineDirective,
                                        size_t activeLineAfterDirective,
                                        size_t activeAfterDirectiveIdx) {
  if (!sawLineDirective)
    return stringutils::countNonSplicedNewlines(prefix, 0, lineStart) + 1;

  return activeLineAfterDirective +
         stringutils::countNonSplicedNewlines(prefix, activeAfterDirectiveIdx,
                                             lineStart);
}

// A source-authored line-control directive is parsed after phase 2/3
// translation, but the physical lines consumed by its original spelling still
// affect the logical line number seen by the next source line.  For example:
//
//   #line 950 \
//   "f.c"
//   int x = __LINE__;
//
// parses as `#line 950 "f.c"`, yet Clang assigns the following `int` to
// logical line 951 because the directive occupied two physical source lines.
// The parsed operand supplies the base line; every additional physical line
// consumed by the directive advances the next observable line by one.
static size_t physicalLineControlDirectiveAdjustment(StringRef src,
                                                     size_t lineStart,
                                                     size_t afterLine) {
  const size_t physicalNewlines =
      stringutils::countNewlines(src, lineStart, afterLine);
  if (physicalNewlines == 0)
    return 0;
  return physicalNewlines - 1;
}

// Expand only the operand part of a possible line-control directive.  Non-line
// preprocessor directives are harmless: after expansion, ParseLineDirective()
// will reject them and the caller will only use them to update the macro
// environment.
static std::string expandSourceLineControlDirective(
    StringRef line, const LineControlMacroMap &macros,
    size_t logicalLineAtLineStart, StringRef activeFileSpelling) {
  const size_t to = line.size();
  size_t p = 0;
  if (!lineStartsWithHash(line, p, to))
    return line.str();

  // Macro expansion in a line-control directive applies to the operands after
  // the directive introducer.  Support both standard spellings:
  //   #line <pp-tokens>
  //   # <pp-tokens>
  // The parser later validates that expansion produced a line number and an
  // optional filename string literal.
  size_t operandBegin = p;
  bool hasLineKeyword = false;
  if (p + 4 <= to && line.substr(p, 4) == "line" &&
      (p + 4 == to || stringutils::isWs(line[p + 4]))) {
    operandBegin = p + 4;
    hasLineKeyword = true;
  }

  std::string expanded;
  expanded.reserve(line.size());
  expanded += line.substr(0, operandBegin).str();
  expanded += expandLineControlMacros(line.substr(operandBegin), macros,
                                      logicalLineAtLineStart,
                                      activeFileSpelling);

  // Keep non-`#line` directives unchanged.  For `# <tokens>` line-control, the
  // expanded operands are enough; for ordinary directives such as `#define`,
  // parsing will fail and the caller will ignore the result.
  (void)hasLineKeyword;
  return expanded;
}

} // namespace

LineDirectiveInserter::LineDirectiveInserter(bool enabled, StringRef cwd)
    : enabled_(enabled), cwd_(cwd) {}

std::string LineDirectiveInserter::ToAbsolutePath(StringRef spelledPath) const {
  llvm::SmallString<256> path(spelledPath);

  if (!llvm::sys::path::is_absolute(path)) {
    if (cwd_.empty()) {
      llvm::sys::fs::make_absolute(path);
    } else {
      // Use the preprocessor working directory captured for this refold run
      // instead of the process CWD, which may differ during replay/testing.
      llvm::SmallString<256> base(cwd_);
      llvm::sys::path::append(base, path);
      path = base;
    }
  }

  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return std::string(path.str());
}

LineDirectiveLocation LineDirectiveInserter::LogicalLocationAtOffset(
    StringRef src, uint64_t offset, StringRef defaultFileSpelling) {
  const size_t clampedOffset =
      static_cast<size_t>(std::min<uint64_t>(offset, src.size()));
  StringRef prefix = src.take_front(clampedOffset);

  // Recover the active source-authored line-control state by scanning the
  // complete source prefix in source order.  A backward search is insufficient
  // for `#line <n>` with no filename operand, because that directive preserves
  // the previously active logical file.
  std::optional<std::string> activeFile;
  size_t activeLineAfterDirective = 0;
  size_t activeAfterDirectiveIdx = 0;
  bool sawLineDirective = false;

  LineControlMacroMap lineControlMacros;
  for (size_t lineStart = 0; lineStart < prefix.size();) {
    std::string logicalLine;
    size_t afterLine = lineStart;
    if (!collectLineControlLogicalLine(prefix, lineStart, prefix.size(),
                                       logicalLine, afterLine) ||
        afterLine <= lineStart)
      break;

    // Directive recognition, macro replacement in directive operands, and
    // #define replacement-list capture all occur after phase 2 line splicing and
    // phase 3 comment replacement.  Reusing this phase-adjusted spelling keeps
    // the owner-local line-control model aligned with the actual preprocessor
    // without changing any refolding ownership decisions.
    std::string phase3Line =
        replaceCommentsWithWhitespaceForLineControl(StringRef(logicalLine));

    // Source line-control directives are interpreted by the preprocessor after
    // macro expansion of their operands.  Recover the deterministic owner-local
    // macro environment from prior source directives before parsing the current
    // line as `#line ...`.  This evaluator exists only to compute the logical
    // resume point for emitted #line repair; it must not strengthen ownership,
    // choose a different edit, or infer macro state outside this source owner.
    const size_t logicalLineAtLineStart = logicalLineAtSourceOffset(
        prefix, lineStart, sawLineDirective, activeLineAfterDirective,
        activeAfterDirectiveIdx);
    StringRef activeFileForExpansion =
        activeFile ? StringRef(*activeFile) : defaultFileSpelling;
    std::string expandedLine = expandSourceLineControlDirective(
        StringRef(phase3Line), lineControlMacros, logicalLineAtLineStart,
        activeFileForExpansion);
    if (std::optional<LineDirectiveState> state =
            ParseLineDirective(StringRef(expandedLine), 0,
                               expandedLine.size())) {
      sawLineDirective = true;
      if (state->hasFileSpelling)
        activeFile = state->fileSpelling;
      // ParseLineDirective() sees the phase-adjusted logical directive line,
      // so `state->lineAfterDir` is the numeric operand after macro expansion.
      // That operand is not always the line observed by the next physical
      // source line: if the directive spelling itself consumed extra physical
      // lines through line splices or block comments, Clang advances the
      // following source line by that physical span.  Record the adjusted
      // post-directive line here so all later owner-local resync queries use
      // the same line-control state the preprocessor would assign.
      activeLineAfterDirective =
          state->lineAfterDir +
          physicalLineControlDirectiveAdjustment(prefix, lineStart, afterLine);
      // The active line state starts after the whole physical directive,
      // including any source lines consumed by phase-2 splices, not after the
      // temporary expanded spelling used only for operand parsing.
      activeAfterDirectiveIdx = afterLine;
    }

    updateLineControlMacroEnvironment(StringRef(phase3Line), lineControlMacros);

    if (afterLine >= prefix.size())
      break;
    lineStart = afterLine;
  }

  if (sawLineDirective) {
    const size_t delta = stringutils::countNonSplicedNewlines(
        prefix, activeAfterDirectiveIdx, prefix.size());
    StringRef file = activeFile ? StringRef(*activeFile) : defaultFileSpelling;
    return LineDirectiveLocation(file, activeLineAfterDirective + delta);
  }

  return LineDirectiveLocation(defaultFileSpelling,
                               stringutils::lineAtOffset(src, clampedOffset));
}

std::string LineDirectiveInserter::WrapIncludeExpansion(
    StringRef childFileSpelling, StringRef parentFileSpelling,
    size_t parentResumeLineNo, StringRef childBody) const {
  if (!enabled_)
    return childBody.str();

  std::string result;
  result.reserve(childBody.size() + 128);

  result += FormatLineDirective(1, childFileSpelling);
  result += childBody.str();

  // Ensure the resume directive begins on its own line even when the included
  // body did not end with a newline.
  if (!childBody.empty() && childBody.back() != '\n') {
    result += '\n';
  }

  result += FormatLineDirective(parentResumeLineNo, parentFileSpelling);
  return result;
}

std::string LineDirectiveInserter::MaybeAppendResyncAfterReplacement(
    StringRef originalFileText, uint64_t s, uint64_t e, StringRef replacement,
    const LineDirectiveLocation &resumeLoc) const {
  if (!enabled_)
    return replacement.str();

  const size_t resumeLine = resumeLoc.lineNo;
  StringRef fileSpellingForDirective(resumeLoc.fileSpelling);

  // A resync directive is safe only if it rejoins the untouched original file
  // at a physical line boundary. We also allow rejoining before indentation-only
  // tail bytes, because the directive can be inserted before that indentation
  // without being stranded in the middle of a source line.
  auto rejoinsUntouchedTailSafelyAtBOL = [&](uint64_t editEnd) -> bool {
    const size_t n = originalFileText.size();
    const size_t pos = (editEnd >= static_cast<uint64_t>(n))
                           ? n
                           : static_cast<size_t>(editEnd);

    if (pos == n || stringutils::isBOL(originalFileText, pos))
      return true;

    size_t nl = originalFileText.find('\n', pos);
    if (nl == StringRef::npos)
      nl = n;

    return stringutils::isIndentOnly(originalFileText, pos, nl);
  };

  // No line directive is needed when the replacement preserves the original
  // physical newline count across the edited byte range.
  size_t origNl = stringutils::countNewlines(originalFileText, s, e);
  size_t replNl = stringutils::countNewlines(replacement);

  if (origNl == replNl) {
    trace("linedir/local",
          "skip (no newline drift): s={0} e={1} origNl={2} replNl={3} file={4}",
          s, e, origNl, replNl, fileSpellingForDirective);
    return replacement.str();
  }

  // The resume directive points at the logical source location where the
  // untouched suffix begins after the replacement, not merely at its physical
  // source line.
  std::string directive =
      FormatLineDirective(resumeLine, fileSpellingForDirective);

  // Empty replacements can only be replaced by a bare directive when both sides
  // of the deletion are line-safe. Otherwise the directive would be injected
  // into the middle of an existing physical line.
  if (replacement.empty()) {
    if (!stringutils::isBOL(originalFileText, static_cast<size_t>(s)) ||
        !rejoinsUntouchedTailSafelyAtBOL(e)) {
      trace("linedir/local",
            "cannot inject (empty replacement rejoins mid-line original): "
            "resumeLine={0} file={1} start={2} end={3}",
            resumeLine, fileSpellingForDirective, s, e);
      return replacement.str();
    }

    trace("linedir/local",
          "inject (empty replacement): resumeLine={0} file={1}", resumeLine,
          fileSpellingForDirective);
    return directive;
  }

  // If the replacement already ends at BOL, append the directive after it. The
  // untouched original tail must also rejoin safely at a line boundary.
  if (replacement.back() == '\n') {
    if (!rejoinsUntouchedTailSafelyAtBOL(e)) {
      trace("linedir/local",
            "cannot inject at end (replacement rejoins mid-line original): "
            "resumeLine={0} file={1} end={2}",
            resumeLine, fileSpellingForDirective, e);
      return replacement.str();
    }

    // Idempotence: avoid appending the same directive twice when this helper is
    // reached repeatedly for an already-resynced replacement.
    if (replacement.ends_with(directive)) {
      trace("linedir/local",
            "skip (already endsWith directive): resumeLine={0} file={1}",
            resumeLine, fileSpellingForDirective);
      return replacement.str();
    }
    trace("linedir/local", "inject (append at end): resumeLine={0} file={1}",
          resumeLine, fileSpellingForDirective);
    return replacement.str() + directive;
  }

  // Otherwise, the remaining safe insertion points are inside the
  // replacement, immediately before bytes that are proved to be a carried
  // prefix of the untouched original suffix line, or before a trailing
  // indentation-only suffix.
  size_t lastNl = replacement.rfind('\n');
  if (lastNl != StringRef::npos) {
    size_t bol = lastNl + 1;

    // Some token-hunk repairs deliberately absorb the first bytes of the
    // untouched suffix line into the replacement so that lexical adjacency is
    // preserved.  Example shape:
    //
    //     replacement: "...\nint"
    //     original[e:]: " keep = ..."
    //
    // The final "int" is not new edited payload; it is the byte-for-byte
    // prefix of the original suffix line [lineStart(e), e).  If the edit also
    // removed physical lines before that suffix, the correct local resync point
    // is before the carried prefix, not after the entire replacement.  This is
    // a proof, not a formatting preference: the replacement suffix must exactly
    // equal the original line prefix that will be rejoined with original[e:].
    if (e <= originalFileText.size() && bol < replacement.size() &&
        !stringutils::isLineSplice(replacement, lastNl)) {
      const size_t resumePrefixBegin = stringutils::lineStartOffset(
          originalFileText, static_cast<size_t>(e));
      const bool resumePrefixStartsLogicalLine =
          resumePrefixBegin == 0 ||
          !stringutils::isLineSplice(originalFileText, resumePrefixBegin - 1);
      if (resumePrefixBegin < e && resumePrefixStartsLogicalLine) {
        StringRef originalResumePrefix =
            originalFileText.slice(resumePrefixBegin, static_cast<size_t>(e));
        StringRef replacementResumePrefix = replacement.substr(bol);
        if (!originalResumePrefix.empty() &&
            replacementResumePrefix == originalResumePrefix) {
          trace("linedir/local",
                "inject (before carried suffix prefix): resumeLine={0} "
                "file={1} lastNl={2} bol={3} prefix={4}",
                resumeLine, fileSpellingForDirective, lastNl, bol,
                stringutils::showWs(stringutils::clip(replacementResumePrefix,
                                                     80)));

          // Idempotence: if the prefix is already preceded by this exact
          // directive, do not duplicate it.
          if (replacement.substr(0, bol).ends_with(directive)) {
            trace("linedir/local",
                  "skip (directive already present before carried suffix "
                  "prefix): resumeLine={0} file={1}",
                  resumeLine, fileSpellingForDirective);
            return replacement.str();
          }

          std::string res = replacement.substr(0, bol).str();
          res += directive;
          res += replacement.substr(bol).str();
          return res;
        }
      }
    }

    if (stringutils::isIndentOnly(replacement, bol, replacement.size())) {
      if (!rejoinsUntouchedTailSafelyAtBOL(e)) {
        trace("linedir/local",
              "cannot inject before indent-only suffix (replacement rejoins "
              "mid-line original): resumeLine={0} file={1} end={2}",
              resumeLine, fileSpellingForDirective, e);
        return replacement.str();
      }

      trace("linedir/local",
            "inject (between last NL and indent-only suffix): resumeLine={0} "
            "file={1} lastNl={2} bol={3}",
            resumeLine, fileSpellingForDirective, lastNl, bol);

      // Idempotence: if the prior line is already the same directive, do not
      // emit it again before the indentation-only suffix.
      if (replacement.substr(0, bol).ends_with(directive)) {
        trace("linedir/local",
              "skip (directive already present immediately before indent-only "
              "suffix): resumeLine={0} file={1}",
              resumeLine, fileSpellingForDirective);
        return replacement.str();
      }

      std::string res = replacement.substr(0, bol).str();
      res += directive;
      res += replacement.substr(bol).str();
      return res;
    }

    // There is a newline, but the tail after it contains substantive text. A
    // directive inserted there would split replacement text rather than cleanly
    // resume the original file.
    if (inTraceMode()) {
      const size_t start =
          static_cast<size_t>(std::clamp(bol, size_t(0), replacement.size()));
      StringRef replFromBol = replacement.substr(start);
      trace("linedir/local",
            "cannot inject at tail (non-indent suffix): resumeLine={0} "
            "file={1} lastNl={2} tail={3}",
            resumeLine, fileSpellingForDirective, lastNl,
            stringutils::showWs(stringutils::clip(replFromBol, 80)));
    }
  } else {
    // With no newline in the replacement, there is no BOL insertion point for
    // the directive.
    trace("linedir/local",
          "cannot inject (no newline in replacement): resumeLine={0} file={1} "
          "replTail={2}",
          resumeLine, fileSpellingForDirective,
          stringutils::showWs(stringutils::clip(replacement, 80)));
  }

  return replacement.str();
}

std::optional<LineDirectiveState>
LineDirectiveInserter::FindLastLineDirectiveState(StringRef src) {
  if (src.empty())
    return std::nullopt;

  // Scan the full original-prefix text.  This is intentionally not a bounded
  // lookback: source-authored line-control directives establish semantic
  // preprocessor state, so missing an old directive can make `__LINE__` or
  // `__FILE__` replay wrong.
  size_t scanEnd = src.size();
  while (scanEnd > 0 && src[scanEnd - 1] == '\n')
    --scanEnd;

  while (scanEnd > 0) {
    size_t prevNl = stringutils::lastIndexOfChar(src, '\n', scanEnd - 1);
    size_t lineStart = (prevNl == StringRef::npos) ? 0 : prevNl + 1;

    if (auto st = ParseLineDirective(src, lineStart, scanEnd))
      return st;

    if (prevNl == StringRef::npos)
      break;

    scanEnd = prevNl;
    while (scanEnd > 0 && src[scanEnd - 1] == '\n')
      --scanEnd;
  }
  return std::nullopt;
}

std::optional<LineDirectiveState>
LineDirectiveInserter::ParseLineDirective(StringRef src, size_t from,
                                          size_t to) {
  if (from > to || to > src.size())
    return std::nullopt;

  size_t p = from;

  // A preprocessing directive may be preceded by horizontal whitespace.  Do
  // not cross a physical newline; `from/to` already delimit one source line.
  while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
    ++p;

  if (p >= to || src[p] != '#')
    return std::nullopt;
  ++p;

  // Both `#line` and `# line` are accepted spellings.  Clang/GCC also accept
  // the numeric line-control form `# 123 "file"`, so leave `p` at the digits
  // when there is no `line` keyword.
  while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
    ++p;

  if (p + 4 <= to && src.substr(p, 4) == "line") {
    p += 4;
    if (p >= to || !stringutils::isWs(src[p]))
      return std::nullopt;
    while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
      ++p;
  }

  const size_t lineStart = p;
  while (p < to && std::isdigit(static_cast<unsigned char>(src[p])))
    ++p;

  if (p == lineStart)
    return std::nullopt;

  size_t lineAfter;
  if (src.slice(lineStart, p).getAsInteger(10, lineAfter))
    return std::nullopt;

  while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
    ++p;

  // Parse the optional quoted filename operand.  Absence of this operand is
  // semantically meaningful: `#line 200` changes only the logical line number
  // and preserves the active logical file.
  llvm::SmallString<64> fileSpelling;
  bool hasFileSpelling = false;
  if (p < to && src[p] == '"') {
    hasFileSpelling = true;
    ++p; // consume opening quote
    while (p < to) {
      char c = src[p++];
      if (c == '"')
        break;
      if (c == '\\' && p < to) {
        char escaped = src[p++];
        if (escaped == 'x' || escaped == 'X') {
          // #line filename operands are C string literals after macro
          // expansion. Decode \x... to the logical filename byte before
          // formatting a resync directive; otherwise __FILE__ observes the raw
          // source spelling instead of the preprocessing result.
          unsigned value = 0;
          bool sawHex = false;
          while (p < to && std::isxdigit(static_cast<unsigned char>(src[p]))) {
            sawHex = true;
            char h = src[p++];
            value *= 16;
            if (h >= '0' && h <= '9')
              value += static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f')
              value += static_cast<unsigned>(10 + h - 'a');
            else if (h >= 'A' && h <= 'F')
              value += static_cast<unsigned>(10 + h - 'A');
          }
          if (sawHex) {
            fileSpelling.push_back(static_cast<char>(value & 0xff));
          } else {
            fileSpelling.push_back(escaped);
          }
          continue;
        }

        if (escaped >= '0' && escaped <= '7') {
          unsigned value = static_cast<unsigned>(escaped - '0');
          for (unsigned digits = 1;
               digits < 3 && p < to && src[p] >= '0' && src[p] <= '7';
               ++digits) {
            value = value * 8 + static_cast<unsigned>(src[p++] - '0');
          }
          fileSpelling.push_back(static_cast<char>(value & 0xff));
          continue;
        }

        switch (escaped) {
        case '"':
        case '\\':
        case '?':
        case '\'':
          fileSpelling.push_back(escaped);
          break;
        case 'a':
          fileSpelling.push_back('\a');
          break;
        case 'b':
          fileSpelling.push_back('\b');
          break;
        case 'e':
        case 'E':
          fileSpelling.push_back(static_cast<char>(0x1b));
          break;
        case 'f':
          fileSpelling.push_back('\f');
          break;
        case 'n':
          fileSpelling.push_back('\n');
          break;
        case 'r':
          fileSpelling.push_back('\r');
          break;
        case 't':
          fileSpelling.push_back('\t');
          break;
        case 'v':
          fileSpelling.push_back('\v');
          break;
        default:
          fileSpelling.push_back(escaped);
          break;
        }
      } else {
        fileSpelling.push_back(c);
      }
    }
  }

  const size_t afterDirectiveIdx =
      (to < src.size() && src[to] == '\n') ? to + 1 : to;

  return LineDirectiveState(std::string(fileSpelling.str()), lineAfter,
                            afterDirectiveIdx, hasFileSpelling);
}

std::string LineDirectiveInserter::EscapeForLineDirective(StringRef path) {
  if (path.empty())
    return "";

  llvm::SmallString<64> escaped;
  escaped.reserve(path.size());
  for (char c : path) {
    switch (c) {
    case '\\':
      // #line filenames are emitted inside double quotes, so preserve a literal
      // backslash by escaping it in the directive spelling.
      escaped.append("\\\\");
      break;
    case '\"':
      // Keep embedded quotes from terminating the quoted filename.
      escaped.append("\\\"");
      break;
    case '\a':
      escaped.append("\\a");
      break;
    case '\b':
      escaped.append("\\b");
      break;
    case '\f':
      escaped.append("\\f");
      break;
    case '\n':
      // Never place a physical newline inside a #line filename; spell the
      // logical filename byte as a C escape so the directive remains one
      // preprocessing line.
      escaped.append("\\n");
      break;
    case '\r':
      escaped.append("\\r");
      break;
    case '\t':
      escaped.append("\\t");
      break;
    case '\v':
      escaped.append("\\v");
      break;
    case static_cast<char>(0x1b):
      // Clang accepts both \e and \E; use one canonical spelling when replaying
      // an escape byte recovered from a source-only line-control filename.
      escaped.append("\\e");
      break;
    default:
      escaped.push_back(c);
      break;
    }
  }

  return std::string(escaped.str());
}

} // namespace refold
} // namespace clang
