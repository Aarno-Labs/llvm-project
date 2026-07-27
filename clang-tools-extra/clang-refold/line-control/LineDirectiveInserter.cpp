//===--- LineDirectiveInserter.cpp ------------------------------*- C++ -*-===//
//
// Source #line directive formatting, parsing, and local resync helpers.
//
// This file implements LineDirectiveInserter and the small source-authored
// line-control evaluator used to determine the logical file/line state at
// owner-local source offsets.  The implementation is deterministic and
// fail-closed: unsupported directive spellings or macro-expanded line-control
// shapes are reported as unproven rather than guessed.
//
//===----------------------------------------------------------------------===//

#include "line-control/LineDirectiveInserter.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

// Implement the whitespace normalization required by macro stringification.
// `# x` in a replacement list stringifies the *raw* argument after trimming
// leading/trailing horizontal whitespace and collapsing internal whitespace
// runs to one space.  The line-control evaluator needs this for constructs like
// `#line 620 STR(logical_file.c)`.
static std::string collapseWhitespaceForStringification(StringRef text) {
  std::string out;
  bool inWs = false;
  StringRef trimmed = stringutils::trimWsNoLF(text);
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

// Spell the result of stringifying one raw macro argument as a C string
// literal. The later #line filename parser decodes that literal, so this
// routine should preserve the preprocessor spelling contract rather than the
// decoded filename.
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

// Parse a function-like macro invocation argument list well enough for #line
// operands.  Arguments may contain nested parentheses and quoted literals; the
// result is raw, trimmed argument spelling because stringification must see raw
// arguments, while ordinary substitution separately uses expanded arguments.
static std::optional<std::vector<std::string>>
parseMacroArguments(StringRef text, size_t openParen, size_t &afterClose) {
  if (openParen >= text.size() || text[openParen] != '(')
    return std::nullopt;

  std::vector<std::string> args;
  std::string cur;
  unsigned depth = 0;

  for (size_t i = openParen + 1; i < text.size(); ++i) {
    char c = text[i];

    if (c == '"' || c == '\'') {
      size_t literalBegin = i;
      if (!stringutils::skipQuotedLiteral(text, i))
        return std::nullopt;
      cur += text.slice(literalBegin, i);
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
        if (!args.empty() || !stringutils::trimWsNoLF(StringRef(cur)).empty())
          args.push_back(stringutils::trimWsNoLF(StringRef(cur)).str());
        afterClose = i + 1;
        return args;
      }
      --depth;
      cur.push_back(c);
      continue;
    }

    if (c == ',' && depth == 0) {
      args.push_back(stringutils::trimWsNoLF(StringRef(cur)).str());
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
static std::optional<std::string>
parseBalancedParenthesizedContent(StringRef text, size_t openParen,
                                  size_t &afterClose) {
  if (openParen >= text.size() || text[openParen] != '(')
    return std::nullopt;

  std::string content;
  unsigned depth = 0;

  for (size_t i = openParen + 1; i < text.size(); ++i) {
    char c = text[i];

    if (c == '"' || c == '\'') {
      size_t literalBegin = i;
      if (!stringutils::skipQuotedLiteral(text, i))
        return std::nullopt;
      content += text.slice(literalBegin, i);
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
// line-control operands.  After parameter substitution, remove `##` and
// adjacent horizontal padding so pasted numeric/string/file-name fragments can
// be parsed by the normal #line parser.  This stays local to line-control
// recovery and is not used for refolding arbitrary macro programs.
static std::string removeTokenPasteOperators(StringRef text) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    if (stringutils::copyQuotedLiteral(text, i, out))
      continue;

    if (i + 1 < text.size() && text[i] == '#' && text[i + 1] == '#') {
      while (!out.empty() && stringutils::isWsNoLF(out.back()))
        out.pop_back();
      i += 2;
      while (i < text.size() && stringutils::isWsNoLF(text[i]))
        ++i;
      continue;
    }

    out.push_back(text[i++]);
  }

  return out;
}

static std::string
expandLineControlMacros(StringRef text, const LineControlMacroMap &macros,
                        std::unordered_set<std::string> &disabled,
                        size_t logicalLineAtLineStart,
                        StringRef activeFileSpelling);

static std::string substituteLineControlReplacementFragment(
    StringRef repl,
    const std::unordered_map<std::string, std::string> &rawByParam,
    const std::unordered_map<std::string, std::string> &expandedByParam,
    bool variadicArgumentHasTokens, const LineControlMacroMap &macros,
    std::unordered_set<std::string> &disabled, size_t logicalLineAtLineStart,
    StringRef activeFileSpelling);

// Return true iff the replacement-list token [nameBegin, nameEnd) is an
// operand of a token-paste operator.  Macro arguments adjacent to `##` are not
// macro-expanded before substitution; their raw tokens are substituted, the
// paste is formed, and the pasted token is then rescanned.  This is the
// critical preprocessor invariant for source line-control macros such as:
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
  while (before > 0 && stringutils::isWsNoLF(repl[before - 1]))
    --before;
  if (before >= 2 && repl[before - 2] == '#' && repl[before - 1] == '#')
    return true;

  size_t after = nameEnd;
  while (after < repl.size() && stringutils::isWsNoLF(repl[after]))
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
    const LineControlMacroMap &macros,
    std::unordered_set<std::string> &disabled, size_t logicalLineAtLineStart,
    StringRef activeFileSpelling) {
  std::unordered_map<std::string, std::string> rawByParam;
  std::unordered_map<std::string, std::string> expandedByParam;
  bool variadicArgumentHasTokens = false;

  for (size_t i = 0; i < def.params.size(); ++i) {
    std::string raw;
    if (def.variadic && def.params[i] == def.variadicParam) {
      raw = joinRawMacroArguments(rawArgs, i);
      variadicArgumentHasTokens =
          !stringutils::trimWsNoLF(StringRef(raw)).empty();
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
    StringRef repl,
    const std::unordered_map<std::string, std::string> &rawByParam,
    const std::unordered_map<std::string, std::string> &expandedByParam,
    bool variadicArgumentHasTokens, const LineControlMacroMap &macros,
    std::unordered_set<std::string> &disabled, size_t logicalLineAtLineStart,
    StringRef activeFileSpelling) {
  std::string substituted;
  substituted.reserve(repl.size());

  for (size_t i = 0; i < repl.size();) {
    if (stringutils::copyQuotedLiteral(repl, i, substituted))
      continue;

    if (repl[i] == '#') {
      if (i + 1 < repl.size() && repl[i + 1] == '#') {
        substituted += "##";
        i += 2;
        continue;
      }

      size_t j = i + 1;
      while (j < repl.size() && stringutils::isWsNoLF(repl[j]))
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
      // scope.  If the variadic argument is empty, the whole parenthesized
      // token sequence disappears; otherwise the token sequence is substituted
      // using the same rules as the surrounding replacement list.  This is the
      // piece needed for line-control forms such as:
      //   #define LOC(n, ...) n __VA_OPT__(__VA_ARGS__)
      //   #define LOC(n, name, ...) n __VA_OPT__(#name)
      if (name == "__VA_OPT__") {
        size_t callPos = i;
        while (callPos < repl.size() && stringutils::isWsNoLF(repl[callPos]))
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
static std::string
expandLineControlMacros(StringRef text, const LineControlMacroMap &macros,
                        std::unordered_set<std::string> &disabled,
                        size_t logicalLineAtLineStart,
                        StringRef activeFileSpelling) {
  std::string out;
  out.reserve(text.size());

  for (size_t i = 0; i < text.size();) {
    if (stringutils::copyQuotedLiteral(text, i, out))
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
      out += stringutils::quoteLineDirectivePath(activeFileSpelling);
      continue;
    }
    if (name == "__FILE_NAME__") {
      out += stringutils::quoteLineDirectivePath(
          stringutils::pathBasename(activeFileSpelling));
      continue;
    }

    auto found = macros.find(name);
    if (found == macros.end() || disabled.count(name)) {
      out += name;
      continue;
    }

    const LineControlMacroDefinition &def = found->second;
    if (!def.functionLike) {
      // Object-like replacement lists resolve token-paste before the rescan
      // that expands the pasted token. Expanding first would turn
      //
      //   #define A 7
      //   #define A00 700
      //   #define LOC A ## 00
      //
      // into the invalid intermediate spelling `7 ## 00`; the preprocessor
      // instead forms `A00` and only then expands it to `700`.
      disabled.insert(name);
      std::string pasted =
          removeTokenPasteOperators(StringRef(def.replacement));
      out +=
          expandLineControlMacros(StringRef(pasted), macros, disabled,
                                  logicalLineAtLineStart, activeFileSpelling);
      disabled.erase(name);
      continue;
    }

    // At invocation sites, whitespace may appear between the macro name and
    // the argument list.  This differs from definition-site recognition, where
    // `NAME(` must be immediate to define a function-like macro.
    size_t callPos = i;
    while (callPos < text.size() && stringutils::isWsNoLF(text[callPos]))
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

static std::string expandLineControlMacros(StringRef text,
                                           const LineControlMacroMap &macros,
                                           size_t logicalLineAtLineStart,
                                           StringRef activeFileSpelling) {
  std::unordered_set<std::string> disabled;
  return expandLineControlMacros(text, macros, disabled, logicalLineAtLineStart,
                                 activeFileSpelling);
}

static bool lineKeywordAt(StringRef line, size_t pos, size_t to) {
  return pos + 4 <= to && line.substr(pos, 4) == "line" &&
         (pos + 4 == to || stringutils::isWs(line[pos + 4]));
}

static bool readLineControlDirectiveAndOperand(StringRef line,
                                               StringRef &directive,
                                               StringRef &operand) {
  const size_t to = line.size();
  size_t p = 0;
  if (!stringutils::consumeDirectiveHash(line, p, to))
    return false;
  if (!stringutils::consumeIdentifier(line, p, to, directive))
    return false;
  operand = line.substr(p);
  return true;
}

static bool lineControlSpellingIsLineDirective(StringRef line) {
  const size_t to = line.size();
  size_t p = 0;
  if (!stringutils::consumeDirectiveHash(line, p, to))
    return false;

  // Standard spelling: #line <pp-tokens>.  Require a token boundary after
  // "line" so ordinary directives with longer names are not misclassified.
  if (lineKeywordAt(line, p, to))
    return true;

  // GCC/Clang numeric line-control form: # <digits> ["file"].  The actual
  // numeric operand may be macro-produced, but a digit here is enough to
  // classify the directive as line-control for proof gating.
  return p < to && std::isdigit(static_cast<unsigned char>(line[p]));
}

// Update the owner-local macro environment from source directives that precede
// the offset being queried.  Only definitions visible in the same source owner
// are considered; this deliberately avoids importing cross-owner macro state
// into line repair and keeps the proof local to the text being emitted.
static void updateLineControlMacroEnvironment(StringRef line,
                                              LineControlMacroMap &macros) {
  const size_t to = line.size();
  size_t p = 0;
  if (!stringutils::consumeDirectiveHash(line, p, to))
    return;

  StringRef directive;
  if (!stringutils::consumeIdentifier(line, p, to, directive))
    return;

  if (directive == "undef") {
    while (p < to && stringutils::isWsNoLF(line[p]))
      ++p;
    StringRef name;
    if (stringutils::consumeIdentifier(line, p, to, name))
      macros.erase(name.str());
    return;
  }

  if (directive != "define")
    return;

  if (p >= to || !stringutils::isWs(line[p]) || line[p] == '\n')
    return;
  stringutils::skipWsNoLF(line, p, to);

  StringRef name;
  if (!stringutils::consumeIdentifier(line, p, to, name))
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
      while (p < to && stringutils::isWsNoLF(line[p]))
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
        if (!stringutils::consumeIdentifier(line, p, to, param)) {
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

      while (p < to && stringutils::isWsNoLF(line[p]))
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

  def.replacement = stringutils::trimWsNoLF(line.substr(p)).str();
  macros[name.str()] = std::move(def);
}

// Build the preprocessing-logical form of one source line for source-authored
// line-control recovery.
//
// `LogicalLocationAtOffset()` asks a preprocessor question: "after executing
// all source directives before this byte offset, what logical file/line is
// active?"  That question must be answered after the early translation steps
// that affect preprocessing directives.  In particular, backslash-newline
// pairs are removed before directive recognition, so both
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

  auto skipLineSplice = [&](size_t &pos) -> bool {
    return stringutils::skipBackslashNewlineSplice(src, limit, pos);
  };

  for (size_t pos = lineBegin; pos < limit;) {
    if (skipLineSplice(pos))
      continue;

    // Comments are not recognized inside string or character literals.  Still
    // apply backslash-newline splicing while copying the literal, because
    // splices are removed before the preprocessor even sees the directive
    // spelling.
    if (src[pos] == '"' || src[pos] == '\'') {
      const char quote = src[pos];
      logicalLine.push_back(src[pos++]);
      while (pos < limit) {
        if (skipLineSplice(pos))
          continue;
        char c = src[pos++];
        logicalLine.push_back(c);
        if (c == '\\' && pos < limit) {
          if (skipLineSplice(pos))
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
    // the directive.  A complete block comment contributes one whitespace
    // character before directive macro expansion/parsing.
    if (pos + 1 < limit && src[pos] == '/' && src[pos + 1] == '*') {
      logicalLine.push_back(src[pos++]);
      logicalLine.push_back(src[pos++]);
      bool closed = false;
      while (pos < limit) {
        if (skipLineSplice(pos))
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

// A source-authored line-control directive is parsed after backslash-newline
// deletion and comment replacement, but the physical lines consumed by its
// original spelling still affect the logical line number seen by the next
// source line.  For example:
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

static void appendDecodedLineControlFilenameEscape(StringRef src, size_t &p,
                                                   size_t to,
                                                   SmallString<64> &out) {
  char escaped = src[p++];
  if (escaped == 'x' || escaped == 'X') {
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
    out.push_back(static_cast<char>((sawHex ? value : escaped) & 0xff));
    return;
  }

  if (escaped >= '0' && escaped <= '7') {
    unsigned value = static_cast<unsigned>(escaped - '0');
    for (unsigned digits = 1;
         digits < 3 && p < to && src[p] >= '0' && src[p] <= '7'; ++digits)
      value = value * 8 + static_cast<unsigned>(src[p++] - '0');
    out.push_back(static_cast<char>(value & 0xff));
    return;
  }

  switch (escaped) {
  case '"':
  case '\\':
  case '?':
  case '\'':
    out.push_back(escaped);
    break;
  case 'a':
    out.push_back('\a');
    break;
  case 'b':
    out.push_back('\b');
    break;
  case 'e':
  case 'E':
    out.push_back(static_cast<char>(0x1b));
    break;
  case 'f':
    out.push_back('\f');
    break;
  case 'n':
    out.push_back('\n');
    break;
  case 'r':
    out.push_back('\r');
    break;
  case 't':
    out.push_back('\t');
    break;
  case 'v':
    out.push_back('\v');
    break;
  default:
    out.push_back(escaped);
    break;
  }
}

// Parse one logical preprocessing line as a line-control directive.
//
// Expected source line-control shapes:
//
//     #line <digits> ["file"]
//     # line <digits> ["file"]
//     # <digits> ["file"]
//
// Within the quoted file spelling, it decodes C string-literal escapes used by
// line-control filename operands. The optional filename operand is tracked
// separately from an explicitly empty filename string so callers can model
// `#line 200` as preserving the current file spelling.
//
// This namespace-local helper is shared by the member API and the model-backed
// logical-location scanner below; keeping the parser out of the class member
// avoids translation-order dependencies when source-prefix recovery runs before
// the public LineDirectiveInserter methods are defined.
static std::optional<LineDirectiveState>
parseLineDirectiveForLineControl(StringRef src, size_t from, size_t to) {
  if (from > to || to > src.size())
    return std::nullopt;

  size_t p = from;

  // A preprocessing directive may be preceded by horizontal whitespace.  Do
  // not cross a physical newline; `from/to` already delimit one source line.
  stringutils::skipWsNoLF(src, p, to);

  if (p >= to || src[p] != '#')
    return std::nullopt;
  ++p;

  // Both `#line` and `# line` are accepted spellings.  Clang/GCC also accept
  // the numeric line-control form `# 123 "file"`, so leave `p` at the digits
  // when there is no `line` keyword.
  stringutils::skipWsNoLF(src, p, to);

  bool usedLineKeyword = false;
  if (lineKeywordAt(src, p, to)) {
    usedLineKeyword = true;
    p += 4;
    stringutils::skipWsNoLF(src, p, to);
  }

  const size_t lineStart = p;
  while (p < to && std::isdigit(static_cast<unsigned char>(src[p])))
    ++p;

  if (p == lineStart)
    return std::nullopt;

  size_t lineAfter;
  if (src.slice(lineStart, p).getAsInteger(10, lineAfter))
    return std::nullopt;

  stringutils::skipWsNoLF(src, p, to);

  // Parse the optional quoted filename operand.  Absence of this operand is
  // semantically meaningful: `#line 200` changes only the logical line number
  // and preserves the active logical file.
  llvm::SmallString<64> fileSpelling;
  bool hasFileSpelling = false;
  if (p < to && src[p] == '"') {
    hasFileSpelling = true;
    bool closedFileQuote = false;
    ++p; // consume opening quote
    while (p < to) {
      char c = src[p++];
      if (c == '"') {
        closedFileQuote = true;
        break;
      }
      if (c == '\\' && p < to)
        appendDecodedLineControlFilenameEscape(src, p, to, fileSpelling);
      else
        fileSpelling.push_back(c);
    }

    if (!closedFileQuote)
      return std::nullopt;
  }

  stringutils::skipWsNoLF(src, p, to);

  // GNU/Clang line-marker directives emitted by preprocessors can carry
  // numeric flags after the optional filename, e.g. `# 1 "file" 2 3`.  Those
  // flags are not part of standard `#line` and do not change the logical
  // file/line state modeled here, so accept them only for the numeric form.
  if (!usedLineKeyword) {
    while (p < to) {
      if (!std::isdigit(static_cast<unsigned char>(src[p])))
        break;
      while (p < to && std::isdigit(static_cast<unsigned char>(src[p])))
        ++p;
      while (p < to && stringutils::isWsNoLF(src[p]))
        ++p;
    }
  }

  if (p != to)
    return std::nullopt;

  const size_t afterDirectiveIdx =
      (to < src.size() && src[to] == '\n') ? to + 1 : to;

  return LineDirectiveState(std::string(fileSpelling.str()), lineAfter,
                            afterDirectiveIdx, hasFileSpelling);
}

// Expand only the operand part of a possible line-control directive.  Non-line
// preprocessor directives are harmless: after expansion, the line-directive
// parser will reject them and the caller will only use them to update the macro
// environment.
static std::string expandSourceLineControlDirective(
    StringRef line, const LineControlMacroMap &macros,
    size_t logicalLineAtLineStart, StringRef activeFileSpelling) {
  const size_t to = line.size();
  size_t p = 0;
  if (!stringutils::consumeDirectiveHash(line, p, to))
    return line.str();

  // Macro expansion in a line-control directive applies to the operands after
  // the directive introducer.  Support both standard spellings:
  //   #line <pp-tokens>
  //   # <pp-tokens>
  // The parser later validates that expansion produced a line number and an
  // optional filename string literal.
  size_t operandBegin = p;
  if (lineKeywordAt(line, p, to))
    operandBegin = p + 4;

  std::string expanded;
  expanded.reserve(line.size());
  expanded += line.substr(0, operandBegin);
  expanded +=
      expandLineControlMacros(line.substr(operandBegin), macros,
                              logicalLineAtLineStart, activeFileSpelling);

  // Keep non-`#line` directives unchanged.  For `# <tokens>` line-control, the
  // expanded operands are enough; for ordinary directives such as `#define`,
  // parsing will fail and the caller will ignore the result.
  return expanded;
}

// Return the original-source byte of the directive-introducing '#'.  The
// logical line scanner has already applied backslash-newline splicing for
// recognition, but the refold map records conditional group boundaries in
// original byte space.
// This bridge consumes horizontal whitespace, physical splice pairs, and block
// comments before the '#', matching the early preprocessing steps used for
// directive recognition while preserving the original byte coordinate.
static std::optional<uint64_t>
findLineControlDirectiveHashOffset(StringRef src, size_t lineStart,
                                   size_t afterLine) {
  size_t p = lineStart;
  while (p < afterLine && p < src.size()) {
    if (stringutils::isWsNoLF(src[p])) {
      ++p;
      continue;
    }

    if (stringutils::skipBackslashNewlineSplice(src, afterLine, p))
      continue;

    if (p + 1 < afterLine && src[p] == '/' && src[p + 1] == '*') {
      p += 2;
      bool closed = false;
      while (p + 1 < afterLine) {
        if (stringutils::skipBackslashNewlineSplice(src, afterLine, p))
          continue;
        if (src[p] == '*' && src[p + 1] == '/') {
          p += 2;
          closed = true;
          break;
        }
        ++p;
      }
      if (!closed)
        return std::nullopt;
      continue;
    }

    if (p + 1 < afterLine && src[p] == '/' && src[p + 1] == '/')
      return std::nullopt;

    if (src[p] == '#')
      return static_cast<uint64_t>(p);
    return std::nullopt;
  }
  return std::nullopt;
}

enum class LineControlDirectiveActivity { Active, Inactive, Unknown };

// Producer-proven conditional activity gate for source line-control effects.
//
// `CondArm::selected` is a PP-material witness, not a direct directive-effect
// witness.  That distinction creates three cases for a source-authored #line
// directive inside a conditional group:
//
//   * Active:   every enclosing group has a selected arm, and the directive
//   byte
//               lies inside that selected arm.
//   * Inactive: some enclosing group has a selected arm, but the directive byte
//               lies outside it.  The preprocessor did not execute this
//               directive, so it must be ignored without poisoning later
//               resync proof.
//   * Unknown:  an enclosing group contains the directive, but the refold map
//   has
//               no selected PP-material arm for that group.  This happens when
//               the selected branch executed only directive effects such as
//               #line and produced no PP tokens.  The consumer cannot prove
//               which #line executed from CondArm::selected alone, so callers
//               must fail closed or preserve the real source line-control
//               stream rather than emit an inferred physical fallback.
//
// The consumer must not re-evaluate #if expressions here: the producer already
// ran Clang's preprocessor with the correct macro state, target semantics,
// feature predicates, include search state, and conditional short-circuit
// rules.
static LineControlDirectiveActivity classifyLineControlDirectiveActivity(
    const RefoldModel &model, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, uint64_t directiveHashOffset) {
  LineControlDirectiveActivity result = LineControlDirectiveActivity::Active;

  for (const RefoldModel::CondGroup &group : model.GetConds()) {
    if (group.file != ownerFile || group.parentIncludeId != ownerIncludeId)
      continue;
    if (!group.ContainsByte(directiveHashOffset))
      continue;

    bool sawSelectedArm = false;
    bool selectedArmContainsDirective = false;
    for (const RefoldModel::CondArm &arm : group.arms) {
      if (!arm.selected)
        continue;
      sawSelectedArm = true;
      if (arm.ContainsByte(directiveHashOffset)) {
        selectedArmContainsDirective = true;
        break;
      }
    }

    if (selectedArmContainsDirective)
      continue;

    if (sawSelectedArm)
      return LineControlDirectiveActivity::Inactive;

    // The group is known, but no branch has a PP-material selection witness.
    // Do not infer line-control effects from source text in this case.
    result = LineControlDirectiveActivity::Unknown;
  }

  return result;
}

/// Return true iff the refold map contains the producer-observed macro-state
/// directive on this logical source line.
///
/// Conditional-arm `selected` metadata is a PP-material witness: it is true
/// when an arm contributed A-side tokens, not merely when Clang selected that
/// branch.  A selected arm that only performs `#define`/`#undef` therefore has
/// no selected-arm PP span, but its macro-state directive is still represented
/// explicitly in the refold map as a MacroDirective item.  Line-control
/// recovery must use that item as the activity proof for source-authored macro
/// state; otherwise live branch-local definitions such as `#define LOC ...` are
/// suppressed before a later `#line LOC` resync.
static bool lineControlMacroDirectiveWasProducerObserved(
    const RefoldModel &model, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, uint64_t directiveHashOffset,
    uint64_t afterLine) {
  for (const RefoldModel::MacroDirective &directive :
       model.GetMacroDirectives()) {
    if (directive.sitePath != ownerFile ||
        directive.ownerIncludeId != ownerIncludeId)
      continue;

    // MacroDirective::siteB is the macro-name byte for a #define/#undef item,
    // not necessarily the directive-introducing '#'.  The directive is a match
    // when its recorded site lies on the same logical directive line whose '#'
    // the scanner is currently processing.  This keeps the proof
    // owner-polymorphic and avoids re-evaluating the surrounding #if.
    if (directiveHashOffset <= directive.siteB && directive.siteB < afterLine)
      return true;
  }

  return false;
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

static LineDirectiveLocation
logicalLocationAtOffsetImpl(StringRef src, uint64_t offset,
                            StringRef defaultFileSpelling,
                            const RefoldModel &model, StringRef ownerFile,
                            std::optional<uint64_t> ownerIncludeId) {
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
  bool sawUnprovenLineControlDirective = false;
  std::optional<uint64_t> lastUnprovenLineControlDirectiveOffset;

  LineControlMacroMap lineControlMacros;
  for (size_t lineStart = 0; lineStart < prefix.size();) {
    std::string logicalLine;
    size_t afterLine = lineStart;
    if (!collectLineControlLogicalLine(prefix, lineStart, prefix.size(),
                                       logicalLine, afterLine) ||
        afterLine <= lineStart) {
      break;
    }

    // Directive recognition, macro replacement in directive operands, and
    // #define replacement-list capture all occur after backslash-newline
    // deletion and comment replacement.  Reusing this directive-logical
    // spelling keeps the owner-local line-control model aligned with the actual
    // preprocessor without changing any refolding ownership decisions.
    std::string directiveLogicalLine =
        stringutils::replaceCommentsWithWhitespacePreservingLiterals(
            StringRef(logicalLine));

    // Source line-control directives are interpreted by the preprocessor after
    // macro expansion of their operands.  Recover the deterministic owner-local
    // macro environment from producer-observed source directives before parsing
    // the current line as `#line ...`. This recovery exists only to compute the
    // logical resume point for emitted #line repair; it must not strengthen
    // ownership, choose a different edit, or infer macro state outside this
    // source owner.
    const size_t logicalLineAtLineStart = logicalLineAtSourceOffset(
        prefix, lineStart, sawLineDirective, activeLineAfterDirective,
        activeAfterDirectiveIdx);
    StringRef activeFileForExpansion =
        activeFile ? StringRef(*activeFile) : defaultFileSpelling;

    std::optional<uint64_t> hashOffset =
        findLineControlDirectiveHashOffset(src, lineStart, afterLine);

    StringRef directive;
    StringRef operand;
    const bool hasNamedDirective = readLineControlDirectiveAndOperand(
        StringRef(directiveLogicalLine), directive, operand);
    const bool isMacroStateDirective =
        hasNamedDirective && (directive == "define" || directive == "undef");
    const bool isLineControlDirectiveSpelling =
        lineControlSpellingIsLineDirective(StringRef(directiveLogicalLine));

    bool directiveEffectsAreActive = false;
    LineControlDirectiveActivity lineDirectiveActivity =
        LineControlDirectiveActivity::Inactive;
    if (hashOffset) {
      if (isMacroStateDirective) {
        directiveEffectsAreActive =
            lineControlMacroDirectiveWasProducerObserved(
                model, ownerFile, ownerIncludeId, *hashOffset,
                static_cast<uint64_t>(afterLine));
      } else {
        lineDirectiveActivity = classifyLineControlDirectiveActivity(
            model, ownerFile, ownerIncludeId, *hashOffset);
        directiveEffectsAreActive =
            lineDirectiveActivity == LineControlDirectiveActivity::Active;
      }
    }

    // Inactive conditional arms are still scanned as text, but their
    // #define/#undef/#line effects are not executed by the preprocessor.
    // Applying those directives here would pollute the owner-local macro
    // environment and recover line-control states that no real preprocessing
    // execution could observe.
    if (directiveEffectsAreActive) {
      std::string expandedLine = expandSourceLineControlDirective(
          StringRef(directiveLogicalLine), lineControlMacros,
          logicalLineAtLineStart, activeFileForExpansion);
      if (std::optional<LineDirectiveState> state =
              parseLineDirectiveForLineControl(StringRef(expandedLine), 0,
                                               expandedLine.size())) {
        sawLineDirective = true;
        if (state->hasFileSpelling)
          activeFile = state->fileSpelling;
        // the line-directive parser sees the directive-logical line,
        // so `state->lineAfterDir` is the numeric operand after macro
        // expansion. That operand is not always the line observed by the next
        // physical source line: if the directive spelling itself consumed extra
        // physical lines through line splices or block comments, Clang advances
        // the following source line by that physical span.  Record the adjusted
        // post-directive line here so all later owner-local resync queries use
        // the same line-control state the preprocessor would assign.
        activeLineAfterDirective =
            state->lineAfterDir + physicalLineControlDirectiveAdjustment(
                                      prefix, lineStart, afterLine);
        // The active line state starts after the whole physical directive,
        // including any source lines consumed by backslash-newline splices, not
        // after the temporary expanded spelling used only for operand parsing.
        activeAfterDirectiveIdx = afterLine;
      } else if (isLineControlDirectiveSpelling && hashOffset) {
        // The directive is syntactically line-control and lies on a
        // producer-active path, but this owner-local scan could not
        // expand/parse its operands. Typical examples are #line operands that
        // depend on macro state imported from a prior include.  Do not replace
        // that real source semantics with a physical fallback #line later;
        // report the recovered location as unproven so the caller can avoid
        // emitting a synthetic override.
        sawUnprovenLineControlDirective = true;
        lastUnprovenLineControlDirectiveOffset = *hashOffset;
      }

      updateLineControlMacroEnvironment(StringRef(directiveLogicalLine),
                                        lineControlMacros);
    } else if (isLineControlDirectiveSpelling && hashOffset &&
               lineDirectiveActivity == LineControlDirectiveActivity::Unknown) {
      // The source prefix contains a line-control directive in a conditional
      // region whose directive-effect activity is not represented by the
      // current model facts.  This is distinct from a proven-inactive arm: an
      // inactive #line must be ignored, while an unknown #line means the active
      // branch may have produced only line-control effects and no PP tokens.
      // Mark only the latter unproven so ordinary selected-token arms can still
      // emit the required synthetic resync for later preserved __LINE__.
      sawUnprovenLineControlDirective = true;
      lastUnprovenLineControlDirectiveOffset = *hashOffset;
    }

    if (afterLine >= prefix.size())
      break;
    lineStart = afterLine;
  }

  if (sawLineDirective) {
    const size_t delta = stringutils::countNonSplicedNewlines(
        prefix, activeAfterDirectiveIdx, prefix.size());
    StringRef file = activeFile ? StringRef(*activeFile) : defaultFileSpelling;
    return LineDirectiveLocation(file, activeLineAfterDirective + delta,
                                 !sawUnprovenLineControlDirective,
                                 lastUnprovenLineControlDirectiveOffset);
  }

  return LineDirectiveLocation(
      defaultFileSpelling, stringutils::lineAtOffset(src, clampedOffset),
      !sawUnprovenLineControlDirective, lastUnprovenLineControlDirectiveOffset);
}

LineDirectiveLocation LineDirectiveInserter::LogicalLocationAtOffset(
    StringRef src, uint64_t offset, StringRef defaultFileSpelling,
    const RefoldModel &model, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId) {
  return logicalLocationAtOffsetImpl(src, offset, defaultFileSpelling, model,
                                     ownerFile, ownerIncludeId);
}

static bool rejoinsUntouchedTailSafelyAtBOL(StringRef originalFileText,
                                            uint64_t editEnd) {
  const size_t n = originalFileText.size();
  const size_t pos =
      (editEnd >= static_cast<uint64_t>(n)) ? n : static_cast<size_t>(editEnd);

  if (pos == n || stringutils::isBOL(originalFileText, pos))
    return true;

  size_t nl = originalFileText.find('\n', pos);
  if (nl == StringRef::npos)
    nl = n;
  return stringutils::isIndentOnly(originalFileText, pos, nl);
}

static std::string insertLineDirectiveAt(StringRef replacement,
                                         StringRef directive, size_t offset) {
  std::string res = replacement.substr(0, offset).str();
  res += directive;
  res += replacement.substr(offset);
  return res;
}

static std::optional<size_t> findCarriedSuffixPrefixInsertionOffset(
    StringRef originalFileText, uint64_t s, uint64_t e, StringRef replacement,
    size_t replacementPrefixOffset, bool requireDeletedLineFromBOL) {
  if (e > originalFileText.size() ||
      replacementPrefixOffset >= replacement.size())
    return std::nullopt;

  const size_t editBegin = static_cast<size_t>(s);
  const size_t editEnd = static_cast<size_t>(e);
  const size_t resumePrefixBegin =
      stringutils::lineStartOffset(originalFileText, editEnd);
  if (resumePrefixBegin >= editEnd)
    return std::nullopt;

  if (resumePrefixBegin != 0 &&
      stringutils::isLineSplice(originalFileText, resumePrefixBegin - 1))
    return std::nullopt;

  if (requireDeletedLineFromBOL) {
    if (editBegin > resumePrefixBegin)
      return std::nullopt;
    if (!stringutils::isBOL(originalFileText, editBegin))
      return std::nullopt;
    if (stringutils::countNonSplicedNewlines(originalFileText, editBegin,
                                             resumePrefixBegin) == 0)
      return std::nullopt;
  }

  StringRef originalResumePrefix =
      originalFileText.slice(resumePrefixBegin, editEnd);
  if (originalResumePrefix.empty())
    return std::nullopt;
  if (replacement.substr(replacementPrefixOffset) != originalResumePrefix)
    return std::nullopt;
  return replacementPrefixOffset;
}

std::string LineDirectiveInserter::MaybeAppendResyncAfterReplacement(
    StringRef originalFileText, uint64_t s, uint64_t e, StringRef replacement,
    const LineDirectiveLocation &resumeLoc) const {
  if (!enabled_)
    return replacement.str();

  const size_t resumeLine = resumeLoc.lineNo;
  StringRef fileSpellingForDirective(resumeLoc.fileSpelling);

  // No line directive is needed when the replacement preserves the original
  // physical newline count across the edited byte range.
  size_t origNl = stringutils::countNewlines(originalFileText, s, e);
  size_t replNl = stringutils::countNewlines(replacement);

  if (origNl == replNl) {
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
        !rejoinsUntouchedTailSafelyAtBOL(originalFileText, e)) {
      return replacement.str();
    }

    return directive;
  }

  // Token-LCS normalization can express a line deletion as replacing the
  // deleted line plus the first token(s) of the surviving suffix line with the
  // same suffix-line prefix.  If the replacement is exactly that carried
  // prefix, the directive belongs before the replacement because the next
  // untouched slice resumes mid-line.
  if (std::optional<size_t> offset = findCarriedSuffixPrefixInsertionOffset(
          originalFileText, s, e, replacement, /*replacementPrefixOffset=*/0,
          /*requireDeletedLineFromBOL=*/true)) {
    return insertLineDirectiveAt(replacement, directive, *offset);
  }

  // If the replacement already ends at BOL, append the directive after it. The
  // untouched original tail must also rejoin safely at a line boundary.
  if (replacement.back() == '\n') {
    if (!rejoinsUntouchedTailSafelyAtBOL(originalFileText, e)) {
      return replacement.str();
    }

    // Idempotence: avoid appending the same directive twice when this helper is
    // reached repeatedly for an already-resynced replacement.
    if (replacement.ends_with(directive)) {
      return replacement.str();
    }
    return replacement.str() + directive;
  }

  // Otherwise, the remaining safe insertion points are inside the replacement,
  // immediately before bytes that are proved to be a carried prefix of the
  // untouched original suffix line, or before a trailing indentation-only
  // suffix.
  size_t lastNl = replacement.rfind('\n');
  if (lastNl != StringRef::npos) {
    size_t bol = lastNl + 1;

    if (!stringutils::isLineSplice(replacement, lastNl)) {
      if (std::optional<size_t> offset = findCarriedSuffixPrefixInsertionOffset(
              originalFileText, s, e, replacement, bol,
              /*requireDeletedLineFromBOL=*/false)) {
        // Idempotence: if the prefix is already preceded by this exact
        // directive, do not duplicate it.
        if (replacement.substr(0, *offset).ends_with(directive)) {
          return replacement.str();
        }

        return insertLineDirectiveAt(replacement, directive, *offset);
      }
    }

    if (stringutils::isIndentOnly(replacement, bol, replacement.size())) {
      if (!rejoinsUntouchedTailSafelyAtBOL(originalFileText, e)) {
        return replacement.str();
      }

      // Idempotence: if the prior line is already the same directive, do not
      // emit it again before the indentation-only suffix.
      if (replacement.substr(0, bol).ends_with(directive)) {
        return replacement.str();
      }

      return insertLineDirectiveAt(replacement, directive, bol);
    }

    // There is a newline, but the tail after it contains substantive text. A
    // directive inserted there would split replacement text rather than cleanly
    // resume the original file.
  } else {
    // With no newline in the replacement, there is no BOL insertion point for
    // the directive.
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

    if (auto st = parseLineDirectiveForLineControl(src, lineStart, scanEnd))
      return st;

    if (prevNl == StringRef::npos)
      break;

    scanEnd = prevNl;
    while (scanEnd > 0 && src[scanEnd - 1] == '\n')
      --scanEnd;
  }
  return std::nullopt;
}

std::string LineDirectiveInserter::EscapeForLineDirective(StringRef path) {
  return stringutils::escapeLineDirectivePath(path);
}

} // namespace refold
} // namespace clang
