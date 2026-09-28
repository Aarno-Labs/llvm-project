//===--- RefoldLineControlRewrite.cpp ---------------------------*- C++ -*-===//
//
// Source-line directive and trivia helpers.
//
// Include materialization and expansion fallback share these definitions when
// they prove include-closure gap trivia, ask whether an untouched suffix can
// observe the presumed file, or spell the directive that resumes a consumed
// line-control gap.  The implementation lives in one translation unit so every
// caller uses the same deterministic rules.
//
//===----------------------------------------------------------------------===//

#include "line-control/SourceLineDirectiveHelpers.h"

#include "line-control/LineDirectiveInserter.h"
#include "model/RefoldPathIdentity.h"
#include "support/StringUtils.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>

namespace clang {
namespace refold {

bool isWsOrCompleteCommentTrivia(StringRef text) {
  size_t i = 0;
  const size_t n = text.size();

  while (i < n) {
    // Ordinary whitespace is inert and may be skipped byte-for-byte.
    if (stringutils::isWs(text[i])) {
      ++i;
      continue;
    }

    // The only non-whitespace spelling admitted here is a complete comment.
    // A lone '/' or any other token spelling is therefore a hard rejection.
    if (i + 1 >= n || text[i] != '/')
      return false;

    if (text[i + 1] == '*') {
      i += 2;
      bool closed = false;
      while (i + 1 < n) {
        if (text[i] == '*' && text[i + 1] == '/') {
          i += 2;
          closed = true;
          break;
        }
        ++i;
      }

      // Do not treat a malformed block comment as trivia; fail closed so the
      // caller can choose a wider structural fallback or terminal B.
      if (!closed)
        return false;

      continue;
    }

    if (text[i + 1] == '/') {
      i += 2;

      // A line comment is complete at the physical newline, or at EOF for the
      // isolated gap buffer. The caller preserves the original bytes, so the
      // newline, if present, is retained.
      while (i < n && text[i] != '\n')
        ++i;
      if (i < n)
        ++i;

      continue;
    }

    return false;
  }

  return true;
}

bool parseLiteralEmptyConditionalDirectiveLine(StringRef line,
                                               unsigned &depth) {
  if (line.ends_with("\n"))
    line = line.drop_back();
  if (line.ends_with("\r"))
    line = line.drop_back();

  // A trailing backslash would splice this physical line with the next one.
  // That makes the gap non-local, so it is outside the preserved-trivia proof.
  StringRef noTrailingHorizontalWs = line.rtrim(" \t\v\f");
  if (noTrailingHorizontalWs.ends_with("\\"))
    return false;

  StringRef rest = line.ltrim(" \t\v\f");
  if (!rest.consume_front("#"))
    return false;
  rest = rest.ltrim(" \t\v\f");

  // Keep directive recognition syntactic and state-free. We only need the
  // directive keyword and the remaining literal condition, not macro expansion.
  StringRef keyword = rest.take_while([](char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
           ('0' <= c && c <= '9') || c == '_';
  });
  rest = rest.drop_front(keyword.size()).trim(" \t\v\f");

  auto isLiteralBit = [](StringRef text) { return text == "0" || text == "1"; };

  if (keyword == "if") {
    if (!isLiteralBit(rest))
      return false;
    ++depth;
    return true;
  }

  if (keyword == "elif") {
    return depth > 0 && isLiteralBit(rest);
  }

  if (keyword == "else") {
    return depth > 0 && rest.empty();
  }

  if (keyword == "endif") {
    if (depth == 0 || !rest.empty())
      return false;
    --depth;
    return true;
  }

  return false;
}

bool isPreservableIncludeClosureGapTrivia(StringRef text) {
  if (isWsOrCompleteCommentTrivia(text))
    return true;

  size_t i = 0;
  const size_t n = text.size();
  unsigned depth = 0;
  bool sawConditionalDirective = false;

  auto skipCompleteComment = [&](size_t &cursor) -> bool {
    if (cursor + 1 >= n || text[cursor] != '/')
      return false;

    if (text[cursor + 1] == '*') {
      cursor += 2;
      while (cursor + 1 < n) {
        if (text[cursor] == '*' && text[cursor + 1] == '/') {
          cursor += 2;
          return true;
        }
        ++cursor;
      }
      return false;
    }

    if (text[cursor + 1] == '/') {
      cursor += 2;
      while (cursor < n && text[cursor] != '\n')
        ++cursor;
      if (cursor < n)
        ++cursor;
      return true;
    }

    return false;
  };

  bool atLineStart = true;
  while (i < n) {
    if (stringutils::isWs(text[i])) {
      // Directives are only admitted at physical line start, modulo horizontal
      // whitespace already consumed by the directive parser.
      atLineStart = text[i] == '\n';
      ++i;
      continue;
    }

    if (text[i] == '/') {
      const size_t before = i;
      if (!skipCompleteComment(i))
        return false;

      // A line comment consumes its terminating newline when present, so the
      // next non-whitespace byte is physically at the start of a new line. A
      // block comment may contain newlines too; preserve directive legality by
      // checking the skipped spelling.
      StringRef skipped = text.slice(before, i);
      atLineStart = skipped.ends_with("\n");
      continue;
    }

    // Do not accept a '#' embedded after real spelling on the same line. That
    // would not be a preprocessing directive in the preserved source.
    if (!atLineStart && text[i] != '#')
      return false;

    size_t lineEnd = i;
    while (lineEnd < n && text[lineEnd] != '\n')
      ++lineEnd;
    if (lineEnd < n)
      ++lineEnd;

    // Only balanced, empty, literal conditional-control directives are
    // preservable. All other directives are macro/preprocessor state and must
    // remain outside this include-closure proof.
    if (!parseLiteralEmptyConditionalDirectiveLine(text.slice(i, lineEnd),
                                                   depth)) {
      return false;
    }

    sawConditionalDirective = true;
    atLineStart = true;
    i = lineEnd;
  }

  return sawConditionalDirective && depth == 0;
}

bool startsWithPreprocessorDirectiveTrivia(StringRef text) {
  StringRef rest = text.ltrim(" \t\v\f");
  return rest.starts_with("#");
}

namespace {

/// Checks whether a file-spelling macro can be observed in a source suffix.
///
/// The model and path identity service are trusted by the line-control proof caller.
/// Cycles, missing callers, and unknown locations stay conservative positives;
/// only a known invocation in another file with no caller chain is ruled out.
class LineControlSuffixInvocationResolver {
public:
  LineControlSuffixInvocationResolver(
      const RefoldModel &model, StringRef file, uint64_t resumeOffset,
      const RefoldPathIdentity &paths)
      : model_(model), file_(file), resumeOffset_(resumeOffset),
        paths_(paths) {}

  /// Starts one cycle-detected caller-chain walk for `macro`.
  bool MayOccurInSuffix(const RefoldModel::MacroInvocation &macro) const {
    SmallVector<uint64_t, 8> activeIds;
    return MayOccurInSuffix(macro, activeIds);
  }

private:
  /// Finds a recorded macro invocation by stable model ID.
  const RefoldModel::MacroInvocation *FindMacroById(uint64_t id) const {
    for (const RefoldModel::MacroInvocation &candidate :
         model_.GetMacroInvocations())
      if (candidate.id == id)
        return &candidate;
    return nullptr;
  }

  /// Recursively walks a macro invocation and its recorded caller chain.
  ///
  /// The ordering and fail-closed cases match the old local resolver: direct
  /// suffix hits win first, then caller recursion, then unknown locations.
  bool MayOccurInSuffix(const RefoldModel::MacroInvocation &macro,
                        SmallVectorImpl<uint64_t> &activeIds) const {
    if (std::find(activeIds.begin(), activeIds.end(), macro.id) !=
        activeIds.end())
      return true;

    activeIds.push_back(macro.id);
    auto popAndReturn = [&](bool value) {
      activeIds.pop_back();
      return value;
    };

    if (macro.invFile && !macro.invFile->empty() &&
        paths_.PathsEqual(*macro.invFile, file_)) {
      // Unknown B-offsets and offsets inside the suffix remain observable.
      if (!macro.invB || *macro.invB >= resumeOffset_)
        return popAndReturn(true);
    }

    if (macro.callerMacroId) {
      const RefoldModel::MacroInvocation *caller =
          FindMacroById(*macro.callerMacroId);
      // A missing caller record is not enough evidence to move line control.
      if (!caller)
        return popAndReturn(true);
      return popAndReturn(MayOccurInSuffix(*caller, activeIds));
    }

    // Unknown physical locations are conservative; a known different file with
    // no caller chain is the only non-observable case.
    return popAndReturn(!macro.invFile || macro.invFile->empty());
  }

  const RefoldModel &model_;
  StringRef file_;
  uint64_t resumeOffset_;
  const RefoldPathIdentity &paths_;
};

} // namespace

bool sourceSuffixMayObservePresumedFileSpelling(
    const RefoldModel &model, StringRef file, uint64_t resumeOffset,
    const RefoldPathIdentity &paths, StringRef fileText) {
  LineControlSuffixInvocationResolver suffixInvocationResolver(
      model, file, resumeOffset, paths);

  for (const RefoldModel::MacroInvocation &macro :
       model.GetMacroInvocations()) {
    if (macro.name != "__FILE__" && macro.name != "__FILE_NAME__")
      continue;

    if (suffixInvocationResolver.MayOccurInSuffix(macro))
      return true;
  }

  if (resumeOffset < fileText.size()) {
    StringRef suffix = fileText.substr(static_cast<size_t>(resumeOffset));
    if (suffix.contains("__FILE__") || suffix.contains("__FILE_NAME__"))
      return true;
  }

  return false;
}

std::string
formatSourceLineDirectiveGapResume(const SourceLineDirectiveGapResume &resume) {
  if (resume.lineMarkerFlags.empty())
    return LineDirectiveInserter::FormatLineDirective(resume.lineAtResume,
                                                      resume.fileSpelling);

  std::string result = "# ";
  result += std::to_string(resume.lineAtResume);
  result += " \"";
  result += LineDirectiveInserter::EscapeForLineDirective(resume.fileSpelling);
  result += "\" ";
  result += resume.lineMarkerFlags;
  result += '\n';
  return result;
}

} // namespace refold
} // namespace clang
