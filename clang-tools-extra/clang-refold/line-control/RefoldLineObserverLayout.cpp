//===--- RefoldLineObserverLayout.cpp ---------------------------*- C++ -*-===//
//
// This file contains the line-observer layout realization service.  It repairs
// TU and include-owner source layout when line-state observers such as
// `__LINE__` remain source-spelled but the edited preprocessed stream collapses
// physical line boundaries that a source-spelled observer would still observe.
//
//===----------------------------------------------------------------------===//

#include "line-control/RefoldLineObserverLayout.h"

#include "edit/RefoldTextEditAssembler.h"
#include "include/IncludeSpellingHelpers.h"
#include "line-control/LineControlEditHelpers.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

bool RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
    StringRef invSpanText, const RefoldModel::MacroInvocation &m) {
  if (invSpanText.empty() || m.name.empty())
    return false;

  const size_t n = invSpanText.size();
  size_t i = 0;

  // Match against the spelled callsite prefix, ignoring only leading trivia
  // before the macro name.
  while (i < n && stringutils::isWs(invSpanText[i]))
    i++;

  if (i >= n)
    return false;

  // The callsite must begin with an identifier spelling. This intentionally
  // avoids substring matching inside larger expressions or tokens.
  const char c0 = invSpanText[i];
  if (!stringutils::isIdentStart(c0))
    return false;

  size_t j = i + 1;
  while (j < n && stringutils::isIdentPart(invSpanText[j]))
    j++;

  // The leading identifier must be exactly the invocation's macro name.
  StringRef ident = invSpanText.slice(i, j);
  if (ident != m.name)
    return false;

  // Object-like macros have no required argument-list syntax, so the leading
  // identifier match is enough to identify the callsite prefix.
  if (m.subkind != "func")
    return true;

  // Function-like macros must be followed by an opening parenthesis, with
  // ordinary whitespace allowed between the macro name and '('.
  while (j < n && stringutils::isWs(invSpanText[j]))
    j++;

  return (j < n && invSpanText[j] == '(');
}

/// Return true iff \p text is only horizontal whitespace or newlines, and it
/// contains at least one physical newline.  This is used to prove that a source
/// line containing a line-state observer has been merged left in the edited
/// preprocessed stream: the original source boundary was a pure line separator,
/// but the B-side boundary no longer contains a newline.
static bool isPureWhitespaceContainingNewline(StringRef text) {
  bool sawNewline = false;
  for (char c : text) {
    if (c == '\n' || c == '\r') {
      sawNewline = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v')
      continue;
    return false;
  }
  return sawNewline;
}

/// Return the start offset of the physical line containing \p offset.
static uint64_t physicalLineBegin(StringRef bytes, uint64_t offset) {
  offset = std::min<uint64_t>(offset, bytes.size());
  while (offset > 0 && bytes[static_cast<size_t>(offset - 1)] != '\n' &&
         bytes[static_cast<size_t>(offset - 1)] != '\r')
    --offset;
  return offset;
}

/// Return the end offset, not including the line separator, of the physical
/// line containing \p offset.
static uint64_t physicalLineEndNoSeparator(StringRef bytes, uint64_t offset) {
  offset = std::min<uint64_t>(offset, bytes.size());
  while (offset < bytes.size() && bytes[static_cast<size_t>(offset)] != '\n' &&
         bytes[static_cast<size_t>(offset)] != '\r')
    ++offset;
  return offset;
}

/// Return the source-byte construct that must be considered when a line-state
/// builtin is observed through \p site.
///
/// Direct `__LINE__` uses observe the physical line containing the builtin.
/// When the builtin is reached through a user macro call, however, the visible
/// source construct is the callsite, not the builtin token in the macro body or
/// argument.  A function-like callsite may span multiple physical lines (for
/// example `ID(\n  __LINE__\n)`), so line-layout proofs must cover the first
/// source line containing the visible invocation through the line containing
/// the end of that invocation.  The returned end is line-end exclusive and does
/// not include the trailing newline separator.
static std::pair<uint64_t, uint64_t>
lineObserverConstructSpan(StringRef bytes,
                          const RefoldModel::MacroInvocation &site) {
  const uint64_t begin = physicalLineBegin(bytes, site.invB.value_or(0));
  const uint64_t endProbe = site.invE.value_or(site.invB.value_or(0));
  const uint64_t end = physicalLineEndNoSeparator(bytes, endProbe);
  return {begin, end};
}

static bool lineObserverCoverIsTokenPreserved(
    const RefoldModel::MacroInvocation &macro, ArrayRef<int64_t> abTokMapA2B,
    ArrayRef<int64_t> abTokMapB2A, ArrayRef<PPTok> aToks,
    ArrayRef<PPTok> bToks) {
  int64_t previousB = -1;
  for (uint64_t aTok = macro.cover.begin; aTok < macro.cover.end; ++aTok) {
    if (aTok >= static_cast<uint64_t>(abTokMapA2B.size()) ||
        aTok >= static_cast<uint64_t>(aToks.size()))
      return false;

    const int64_t bTok = abTokMapA2B[static_cast<size_t>(aTok)];
    if (bTok < 0 || bTok >= static_cast<int64_t>(bToks.size()) ||
        static_cast<uint64_t>(bTok) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(bTok)] != static_cast<int64_t>(aTok) ||
        aToks[static_cast<size_t>(aTok)].spelling !=
            bToks[static_cast<size_t>(bTok)].spelling ||
        (previousB >= 0 && bTok != previousB + 1))
      return false;

    previousB = bTok;
  }
  return true;
}

struct LineObserverConstructTokens {
  uint64_t firstATok = 0;
  uint64_t lastATok = 0;
  size_t firstBTok = 0;
  size_t lastBTok = 0;
  uint64_t firstBBegin = 0;
  uint64_t bEnd = 0;
};

static std::optional<LineObserverConstructTokens> lineObserverConstructTokens(
    StringRef ownerFile, uint64_t constructBegin, uint64_t constructEnd,
    const RefoldModel::MacroInvocation &macro, const RefoldPathIdentity &paths,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool requirePreviousATok) {
  std::optional<uint64_t> firstATok;
  std::optional<uint64_t> lastATok;
  for (uint64_t aTok = 0; aTok < static_cast<uint64_t>(aToks.size()); ++aTok) {
    auto it = tokmapByPP.find(aTok);
    if (it == tokmapByPP.end())
      continue;
    const RefoldModel::TokMapEntry &entry = it->second;
    if (!paths.PathsEqual(entry.file, ownerFile))
      continue;
    if (entry.b < constructBegin || entry.e > constructEnd)
      continue;
    if (!firstATok || aTok < *firstATok)
      firstATok = aTok;
    if (!lastATok || aTok > *lastATok)
      lastATok = aTok;
  }

  if (!firstATok || !lastATok)
    return std::nullopt;
  if (requirePreviousATok && *firstATok == 0)
    return std::nullopt;
  if (macro.cover.begin < *firstATok || macro.cover.end - 1 > *lastATok)
    return std::nullopt;
  if (*firstATok >= static_cast<uint64_t>(abTokMapA2B.size()) ||
      *lastATok >= static_cast<uint64_t>(abTokMapA2B.size()))
    return std::nullopt;

  const int64_t firstBTokI = abTokMapA2B[static_cast<size_t>(*firstATok)];
  const int64_t lastBTokI = abTokMapA2B[static_cast<size_t>(*lastATok)];
  if (firstBTokI < 0 || lastBTokI < firstBTokI ||
      firstBTokI >= static_cast<int64_t>(bTokOff.size()) ||
      lastBTokI >= static_cast<int64_t>(bTokOff.size()) ||
      lastBTokI >= static_cast<int64_t>(bToks.size()))
    return std::nullopt;

  const size_t firstBTok = static_cast<size_t>(firstBTokI);
  const size_t lastBTok = static_cast<size_t>(lastBTokI);
  return LineObserverConstructTokens{
      *firstATok,
      *lastATok,
      firstBTok,
      lastBTok,
      static_cast<uint64_t>(bTokOff[firstBTok]),
      static_cast<uint64_t>(bTokOff[lastBTok] +
                            bToks[lastBTok].spelling.size())};
}

struct LineObserverCollapsedGap {
  uint64_t leftATok = 0;
  uint64_t rightATok = 0;
  const RefoldModel::TokMapEntry *leftEntry = nullptr;
  size_t leftBTok = 0;
  uint64_t leftBEnd = 0;
};

static std::optional<LineObserverCollapsedGap> findLineObserverCollapsedPreGap(
    StringRef ownerBytes, StringRef ownerFile, StringRef bSource,
    uint64_t searchBegin, uint64_t observerLineBegin,
    uint64_t firstObserverATok, const RefoldPathIdentity &paths,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff) {
  for (uint64_t aTok = 1; aTok < firstObserverATok; ++aTok) {
    const uint64_t leftATok = aTok - 1;
    auto leftIt = tokmapByPP.find(leftATok);
    auto rightIt = tokmapByPP.find(aTok);
    if (leftIt == tokmapByPP.end() || rightIt == tokmapByPP.end())
      continue;

    const RefoldModel::TokMapEntry &leftEntry = leftIt->second;
    const RefoldModel::TokMapEntry &rightEntry = rightIt->second;
    if (!paths.PathsEqual(leftEntry.file, ownerFile) ||
        !paths.PathsEqual(rightEntry.file, ownerFile))
      continue;
    if (leftEntry.e < searchBegin || rightEntry.b > observerLineBegin)
      continue;
    if (leftEntry.e > rightEntry.b)
      continue;
    if (leftATok >= static_cast<uint64_t>(abTokMapA2B.size()) ||
        aTok >= static_cast<uint64_t>(abTokMapA2B.size()))
      continue;

    const int64_t leftBTokI = abTokMapA2B[static_cast<size_t>(leftATok)];
    const int64_t rightBTokI = abTokMapA2B[static_cast<size_t>(aTok)];
    if (leftBTokI < 0 || rightBTokI < 0 ||
        static_cast<uint64_t>(leftBTokI) >= abTokMapB2A.size() ||
        static_cast<uint64_t>(rightBTokI) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(leftBTokI)] !=
            static_cast<int64_t>(leftATok) ||
        abTokMapB2A[static_cast<size_t>(rightBTokI)] !=
            static_cast<int64_t>(aTok))
      continue;
    if (static_cast<size_t>(leftBTokI) >= bTokOff.size() ||
        static_cast<size_t>(rightBTokI) >= bTokOff.size() ||
        static_cast<size_t>(leftBTokI) >= bToks.size())
      continue;

    const uint64_t leftBEnd = static_cast<uint64_t>(
        bTokOff[static_cast<size_t>(leftBTokI)] +
        bToks[static_cast<size_t>(leftBTokI)].spelling.size());
    const uint64_t rightBBegin =
        static_cast<uint64_t>(bTokOff[static_cast<size_t>(rightBTokI)]);
    if (rightBBegin < leftBEnd || rightBBegin > bSource.size())
      continue;

    StringRef sourceGap = ownerBytes.slice(leftEntry.e, rightEntry.b);
    StringRef bGap = bSource.slice(leftBEnd, rightBBegin);
    if (!isPureWhitespaceContainingNewline(sourceGap))
      continue;
    if (bGap.contains('\n') || bGap.contains('\r'))
      continue;

    return LineObserverCollapsedGap{leftATok, aTok, &leftEntry,
                                    static_cast<size_t>(leftBTokI), leftBEnd};
  }

  return std::nullopt;
}

/// Proven right-extension of a line-observer materialization envelope.
///
/// The own-line merge theorem starts from a closed owner-local interval ending
/// at the physical line that contains the observable line-state construct.  If
/// B also removed the newline after that construct line, stopping there leaves
/// a suffix token stranded on its original source line even though B placed it
/// on the same physical line.  This helper extends the interval across each
/// immediately following owner-local source line whose separating newline was
/// removed in B.  Every extended line must be token-mapped, token-contiguous in
/// B, and contain only whitespace outside the mapped tokens, so the resulting
/// byte edit is a deterministic closed B-line envelope rather than a heuristic
/// layout grab.
struct LineObserverRightClosure {
  uint64_t sourceEnd = 0;
  uint64_t bEnd = 0;
  uint64_t lastATok = 0;
  size_t lastBTok = 0;

  // Source-spelled text for any right-extension lines appended after the
  // initial observer construct.  Prefix-collapse repairs preserve the observer
  // source spelling, so they cannot materialize these right-side lines from B
  // without replacing `__LINE__` by its numeric expansion. Instead, each
  // extension records the B inter-token gap that removed the physical newline,
  // followed by the original source spelling of the next owner-local line.
  // Own-line materialization ignores this field because it emits the whole
  // closed envelope from B.
  std::string sourceSpelledExtension;
};

static std::optional<LineObserverRightClosure> extendLineObserverRightClosure(
    StringRef ownerBytes, StringRef ownerFile, StringRef bSource,
    const RefoldPathIdentity &paths,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, uint64_t sourceEnd,
    uint64_t bEnd, uint64_t lastATok, size_t lastBTok) {
  LineObserverRightClosure closure{sourceEnd, bEnd, lastATok, lastBTok, {}};

  auto mappedBTok = [&](uint64_t aTok) -> std::optional<size_t> {
    if (aTok >= static_cast<uint64_t>(abTokMapA2B.size()))
      return std::nullopt;
    const int64_t bTokI = abTokMapA2B[static_cast<size_t>(aTok)];
    if (bTokI < 0 || static_cast<uint64_t>(bTokI) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(bTokI)] != static_cast<int64_t>(aTok))
      return std::nullopt;
    const size_t bTok = static_cast<size_t>(bTokI);
    if (bTok >= bTokOff.size() || bTok >= bToks.size())
      return std::nullopt;
    return bTok;
  };

  auto bTokenEnd = [&](size_t bTok) -> uint64_t {
    return static_cast<uint64_t>(bTokOff[bTok] + bToks[bTok].spelling.size());
  };

  while (true) {
    const uint64_t nextATok = closure.lastATok + 1;
    auto nextIt = tokmapByPP.find(nextATok);
    if (nextIt == tokmapByPP.end())
      return closure;
    const RefoldModel::TokMapEntry &nextEntry = nextIt->second;
    if (!paths.PathsEqual(nextEntry.file, ownerFile))
      return closure;
    if (nextEntry.b < closure.sourceEnd)
      return std::nullopt;

    StringRef sourceGap = ownerBytes.slice(closure.sourceEnd, nextEntry.b);
    if (!isPureWhitespaceContainingNewline(sourceGap))
      return closure;

    std::optional<size_t> nextBTok = mappedBTok(nextATok);
    if (!nextBTok)
      return std::nullopt;
    const uint64_t nextBBegin = static_cast<uint64_t>(bTokOff[*nextBTok]);
    if (nextBBegin < closure.bEnd || nextBBegin > bSource.size())
      return std::nullopt;
    StringRef bGap = bSource.slice(closure.bEnd, nextBBegin);
    if (bGap.contains('\n') || bGap.contains('\r'))
      return closure;

    const uint64_t lineBegin = physicalLineBegin(ownerBytes, nextEntry.b);
    const uint64_t lineEnd =
        physicalLineEndNoSeparator(ownerBytes, nextEntry.b);
    if (lineBegin < closure.sourceEnd || nextEntry.e > lineEnd)
      return std::nullopt;

    uint64_t cursor = nextEntry.e;
    uint64_t lineLastATok = nextATok;
    size_t lineLastBTok = *nextBTok;
    uint64_t lineBEnd = bTokenEnd(lineLastBTok);

    for (uint64_t aTok = nextATok + 1;; ++aTok) {
      auto it = tokmapByPP.find(aTok);
      if (it == tokmapByPP.end())
        break;
      const RefoldModel::TokMapEntry &entry = it->second;
      if (!paths.PathsEqual(entry.file, ownerFile) || entry.b >= lineEnd)
        break;
      if (entry.b < cursor || entry.e > lineEnd)
        return std::nullopt;
      if (!ownerBytes.slice(cursor, entry.b).trim().empty())
        return std::nullopt;

      std::optional<size_t> bTok = mappedBTok(aTok);
      if (!bTok || *bTok != lineLastBTok + 1)
        return std::nullopt;
      const uint64_t bBegin = static_cast<uint64_t>(bTokOff[*bTok]);
      if (bBegin < lineBEnd || bBegin > bSource.size())
        return std::nullopt;
      StringRef intraLineBGap = bSource.slice(lineBEnd, bBegin);
      if (intraLineBGap.contains('\n') || intraLineBGap.contains('\r'))
        return std::nullopt;

      cursor = entry.e;
      lineLastATok = aTok;
      lineLastBTok = *bTok;
      lineBEnd = bTokenEnd(lineLastBTok);
    }

    if (!ownerBytes.slice(cursor, lineEnd).trim().empty())
      return std::nullopt;

    closure.sourceSpelledExtension += bGap.str();
    closure.sourceSpelledExtension +=
        ownerBytes.slice(nextEntry.b, lineEnd).str();
    closure.sourceEnd = lineEnd;
    closure.bEnd = lineBEnd;
    closure.lastATok = lineLastATok;
    closure.lastBTok = lineLastBTok;
  }
}

struct LineObserverOwnLineMerge {
  uint64_t previousATok = 0;
  const RefoldModel::TokMapEntry *prevEntry = nullptr;
  size_t prevBTok = 0;
  uint64_t prevBEnd = 0;
  uint64_t firstBBegin = 0;
  uint64_t initialBEnd = 0;
  LineObserverRightClosure closure;
};

/// Resolve the shared owner-polymorphic proof for the case where B removes the
/// physical newline immediately before a preserved `__LINE__` observer.  TU and
/// header materialization lower the resulting envelope differently, but the
/// closure obligation is identical: previous owner token, source newline gap,
/// B non-newline gap, preserved observer envelope, and any right-extension
/// lines must all be token-map closed.
static std::optional<LineObserverOwnLineMerge> resolveLineObserverOwnLineMerge(
    StringRef ownerBytes, StringRef ownerFile, StringRef bSource,
    const RefoldPathIdentity &paths,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, uint64_t sourceLineBegin,
    uint64_t sourceLineEnd, const LineObserverConstructTokens &observerTokens) {
  if (observerTokens.firstATok == 0)
    return std::nullopt;

  const uint64_t previousATok = observerTokens.firstATok - 1;
  auto prevIt = tokmapByPP.find(previousATok);
  if (prevIt == tokmapByPP.end())
    return std::nullopt;
  const RefoldModel::TokMapEntry &prevEntry = prevIt->second;
  if (!paths.PathsEqual(prevEntry.file, ownerFile))
    return std::nullopt;
  if (prevEntry.e > sourceLineBegin)
    return std::nullopt;

  StringRef originalSeparator = ownerBytes.slice(prevEntry.e, sourceLineBegin);
  if (!isPureWhitespaceContainingNewline(originalSeparator))
    return std::nullopt;

  if (previousATok >= static_cast<uint64_t>(abTokMapA2B.size()))
    return std::nullopt;
  const int64_t prevBTokI = abTokMapA2B[static_cast<size_t>(previousATok)];
  if (prevBTokI < 0)
    return std::nullopt;
  const size_t prevBTok = static_cast<size_t>(prevBTokI);
  if (prevBTok >= bTokOff.size() || prevBTok >= bToks.size())
    return std::nullopt;

  const uint64_t prevBEnd = static_cast<uint64_t>(
      bTokOff[prevBTok] + bToks[prevBTok].spelling.size());
  const uint64_t firstBBegin = observerTokens.firstBBegin;
  if (firstBBegin < prevBEnd || firstBBegin > bSource.size())
    return std::nullopt;
  StringRef bSeparator = bSource.slice(prevBEnd, firstBBegin);
  if (bSeparator.contains('\n') || bSeparator.contains('\r'))
    return std::nullopt;

  const uint64_t initialBEnd = observerTokens.bEnd;
  if (initialBEnd < prevBEnd || initialBEnd > bSource.size())
    return std::nullopt;

  std::optional<LineObserverRightClosure> closure =
      extendLineObserverRightClosure(
          ownerBytes, ownerFile, bSource, paths, tokmapByPP, abTokMapA2B,
          abTokMapB2A, bToks, bTokOff, sourceLineEnd, initialBEnd,
          observerTokens.lastATok, observerTokens.lastBTok);
  if (!closure)
    return std::nullopt;

  return LineObserverOwnLineMerge{previousATok,       &prevEntry,  prevBTok,
                                  prevBEnd,           firstBBegin, initialBEnd,
                                  std::move(*closure)};
}

/// Source-spelled prefix segment for a collapsed pre-observer B line.
///
/// Prefix-collapse repairs preserve the line-state observer source spelling and
/// emit a canonical #line immediately before it.  The prefix before that repair
/// should therefore be represented as "B layout plus original source
/// constructs" rather than blindly materializing B bytes.  This keeps
/// still-valid macro invocations such as `B_STMT` source-spelled while proving
/// the same B physical-line collapse.
struct LineObserverSourcePrefix {
  std::string text;
  uint64_t sourceEnd = 0;
  uint64_t lastATok = 0;
  size_t lastBTok = 0;
};

/// Return true iff every token in [firstATok,lastATok] maps bijectively to a
/// contiguous B-token range.  The helper reports that B range so callers can
/// use B's inter-construct whitespace while preserving the source spelling of
/// the construct itself.
static bool lineObserverMappedContiguousBRange(
    uint64_t firstATok, uint64_t lastATok, ArrayRef<int64_t> abTokMapA2B,
    ArrayRef<int64_t> abTokMapB2A, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, size_t &firstBTok, size_t &lastBTok) {
  if (firstATok > lastATok || lastATok >= abTokMapA2B.size())
    return false;

  std::optional<size_t> firstB;
  std::optional<size_t> prevB;
  for (uint64_t aTok = firstATok; aTok <= lastATok; ++aTok) {
    const int64_t bTokI = abTokMapA2B[static_cast<size_t>(aTok)];
    if (bTokI < 0 || static_cast<uint64_t>(bTokI) >= abTokMapB2A.size() ||
        abTokMapB2A[static_cast<size_t>(bTokI)] != static_cast<int64_t>(aTok))
      return false;
    const size_t bTok = static_cast<size_t>(bTokI);
    if (bTok >= bTokOff.size() || bTok >= bToks.size())
      return false;
    if (prevB && bTok != *prevB + 1)
      return false;
    if (!firstB)
      firstB = bTok;
    prevB = bTok;
  }

  if (!firstB || !prevB)
    return false;
  firstBTok = *firstB;
  lastBTok = *prevB;
  return true;
}

static const RefoldModel::MacroInvocation *lineObserverSourceMacroConstructAt(
    ArrayRef<RefoldModel::MacroInvocation> macroInvocations, uint64_t aTok,
    uint64_t limitATok, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, const RefoldPathIdentity &paths) {
  const RefoldModel::MacroInvocation *best = nullptr;
  for (const RefoldModel::MacroInvocation &macro : macroInvocations) {
    if (!macro.cover.IsValid() || macro.name == "__LINE__")
      continue;
    if (macro.ownerIncludeId != ownerIncludeId)
      continue;
    if (!macro.invFile || !macro.invB || !macro.invE)
      continue;
    if (!paths.PathsEqual(*macro.invFile, ownerFile))
      continue;
    if (macro.cover.begin != aTok || macro.cover.end > limitATok)
      continue;
    // Prefer the widest source-spelled construct beginning at this token.  This
    // preserves an outer macro call when nested macro calls share the same
    // first produced token, and it is deterministic because macro ids are
    // stable.
    if (!best || macro.cover.end > best->cover.end ||
        (macro.cover.end == best->cover.end && macro.id < best->id)) {
      best = &macro;
    }
  }
  return best;
}

static std::optional<LineObserverSourcePrefix>
buildLineObserverSourceSpelledPrefix(
    StringRef ownerBytes, StringRef ownerFile,
    std::optional<uint64_t> ownerIncludeId, StringRef bSource,
    const RefoldPathIdentity &paths,
    const DenseMap<uint64_t, RefoldModel::TokMapEntry> &tokmapByPP,
    ArrayRef<RefoldModel::MacroInvocation> macroInvocations,
    ArrayRef<int64_t> abTokMapA2B, ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, uint64_t sourceBegin,
    uint64_t bBegin, uint64_t firstATok, uint64_t limitATok,
    uint64_t sourceLimit, uint64_t bLimit) {
  LineObserverSourcePrefix result;
  uint64_t cursor = sourceBegin;
  uint64_t bCursor = bBegin;
  uint64_t aTok = firstATok;

  while (aTok < limitATok) {
    uint64_t constructFirstATok = aTok;
    uint64_t constructLastATok = aTok;
    uint64_t constructBegin = 0;
    uint64_t constructEnd = 0;

    if (const RefoldModel::MacroInvocation *macro =
            lineObserverSourceMacroConstructAt(macroInvocations, aTok,
                                               limitATok, ownerFile,
                                               ownerIncludeId, paths)) {
      constructFirstATok = macro->cover.begin;
      constructLastATok = macro->cover.end - 1;
      constructBegin = *macro->invB;
      constructEnd = *macro->invE;
    } else {
      auto it = tokmapByPP.find(aTok);
      if (it == tokmapByPP.end())
        return std::nullopt;
      const RefoldModel::TokMapEntry &entry = it->second;
      if (!paths.PathsEqual(entry.file, ownerFile))
        return std::nullopt;
      constructBegin = entry.b;
      constructEnd = entry.e;
    }

    if (constructBegin < cursor || constructEnd > sourceLimit)
      return std::nullopt;
    if (!ownerBytes.slice(cursor, constructBegin).trim().empty())
      return std::nullopt;

    size_t constructFirstBTok = 0;
    size_t constructLastBTok = 0;
    if (!lineObserverMappedContiguousBRange(
            constructFirstATok, constructLastATok, abTokMapA2B, abTokMapB2A,
            bToks, bTokOff, constructFirstBTok, constructLastBTok))
      return std::nullopt;

    const uint64_t constructBBegin =
        static_cast<uint64_t>(bTokOff[constructFirstBTok]);
    if (constructBBegin < bCursor || constructBBegin > bSource.size())
      return std::nullopt;
    StringRef bGap = bSource.slice(bCursor, constructBBegin);
    if (bGap.contains('\n') || bGap.contains('\r'))
      return std::nullopt;

    result.text += bGap.str();
    result.text += ownerBytes.slice(constructBegin, constructEnd).str();
    result.sourceEnd = constructEnd;
    result.lastATok = constructLastATok;
    result.lastBTok = constructLastBTok;

    cursor = constructEnd;
    bCursor = static_cast<uint64_t>(bTokOff[constructLastBTok] +
                                    bToks[constructLastBTok].spelling.size());
    aTok = constructLastATok + 1;
  }

  if (!ownerBytes.slice(cursor, sourceLimit).trim().empty())
    return std::nullopt;
  if (bLimit < bCursor || bLimit > bSource.size())
    return std::nullopt;
  result.text += bSource.slice(bCursor, bLimit).str();
  return result;
}

bool RefoldLineObserverLayout::AppendTURealizationEdits(
    StringRef tuPath, StringRef tuBytes, std::vector<TextEdit> &tuEdits) const {
  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  auto editOverlapsExisting = [&](uint64_t begin, uint64_t end) {
    for (const TextEdit &edit : tuEdits) {
      if (begin < edit.end && edit.start < end)
        return true;
    }
    return false;
  };

  const auto &tokmapByPP = model_.GetTokmapByPP();
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        lineControlProof_.LineStateObservableMacroSite(macro);
    if (!site || site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!paths_.PathsEqual(*site->invFile, tuPath))
      continue;
    if (!macro.cover.IsValid())
      continue;

    // Only handle the fully token-preserved case.  If the builtin expansion is
    // already token-changed, ordinary macro materialization owns it.
    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [sourceLineBegin, sourceLineEnd] =
        lineObserverConstructSpan(tuBytes, *site);
    if (sourceLineBegin >= sourceLineEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(tuPath, sourceLineBegin, sourceLineEnd,
                                    macro, paths_, tokmapByPP, abTokMapA2B_,
                                    aToks_, bToks_, bTokOff_,
                                    /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    std::optional<LineObserverOwnLineMerge> ownLineMerge =
        resolveLineObserverOwnLineMerge(tuBytes, tuPath, bSource_, paths_,
                                        tokmapByPP, abTokMapA2B_, abTokMapB2A_,
                                        bToks_, bTokOff_, sourceLineBegin,
                                        sourceLineEnd, *observerTokens);
    if (!ownLineMerge)
      continue;

    const RefoldModel::TokMapEntry &prevEntry = *ownLineMerge->prevEntry;
    const uint64_t prevBEnd = ownLineMerge->prevBEnd;
    const uint64_t initialBEnd = ownLineMerge->initialBEnd;
    const LineObserverRightClosure &ownLineClosure = ownLineMerge->closure;

    const uint64_t editBegin = prevEntry.e;
    const uint64_t editEnd = ownLineClosure.sourceEnd;
    if (editBegin >= editEnd)
      continue;
    if (editOverlapsExisting(editBegin, editEnd)) {
      continue;
    }

    // Materialize only the left/observer part of the B physical-line envelope
    // from B.  Any proven right-extension line is appended from the original
    // owner spelling with B's inter-token gap.  This preserves source-spelled
    // constructs such as object-like macro invocations (`SEMI`) that merely
    // moved onto the same B line; expanding them to their PP token spelling
    // would validate, but would be a less faithful refolding.
    std::string replacement = bSource_.slice(prevBEnd, initialBEnd).str();
    replacement += ownLineClosure.sourceSpelledExtension;
    if (replacement.empty())
      continue;

    TextEdit edit{editBegin,    editEnd, replacement, std::nullopt,
                  std::nullopt, {},      {},          {}};
    textEditAssembler_.CertifyTextEditMaterializedBByteRange(edit, prevBEnd,
                                                             initialBEnd);
    textEditAssembler_.AttachAcceptedResultCarrier(
        edit, proofLattice_.AcceptedCandidateBuilder()
                  .BuildAcceptedTUTextEditCandidate(
                      AcceptedPathKind::TUByteSpanConservativeEdit, editBegin,
                      editEnd, replacement));
    tuEdits.push_back(std::move(edit));
  }

  // A preserved source-spelled __LINE__ observer can remain source-spelled when
  // B preserves the observer line itself, but only if the copied prefix between
  // the dominating #line state and that observer preserves the same physical
  // line count.  A token-identical B edit can still merge two earlier ordinary
  // source lines, leaving the observer on a fresh B line but one logical line
  // earlier if we simply copy the original source.  In that case, realize the
  // collapsed prefix bytes from B and emit a canonical repair immediately
  // before the preserved observer.
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        lineControlProof_.LineStateObservableMacroSite(macro);
    if (!site || site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!paths_.PathsEqual(*site->invFile, tuPath))
      continue;
    if (!macro.cover.IsValid())
      continue;

    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [observerLineBegin, observerConstructEnd] =
        lineObserverConstructSpan(tuBytes, *site);
    if (observerLineBegin == 0 || observerLineBegin >= observerConstructEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(
            tuPath, observerLineBegin, observerConstructEnd, macro, paths_,
            tokmapByPP, abTokMapA2B_, aToks_, bToks_, bTokOff_,
            /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    const uint64_t firstObserverBBegin = observerTokens->firstBBegin;
    const uint64_t observerBEnd = observerTokens->bEnd;

    const std::optional<uint64_t> latestLineControlEnd =
        lineControlProof_.LatestProducerLineControlEndBefore(
            std::nullopt, tuPath, observerLineBegin);
    const uint64_t searchBegin = latestLineControlEnd.value_or(0);

    std::optional<LineObserverCollapsedGap> collapsedGap =
        findLineObserverCollapsedPreGap(
            tuBytes, tuPath, bSource_, searchBegin, observerLineBegin,
            observerTokens->firstATok, paths_, tokmapByPP, abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_);
    if (!collapsedGap)
      continue;

    if (firstObserverBBegin < collapsedGap->leftBEnd ||
        firstObserverBBegin > bSource_.size() || observerBEnd > bSource_.size())
      continue;

    std::optional<LineObserverSourcePrefix> sourcePrefix =
        buildLineObserverSourceSpelledPrefix(
            tuBytes, tuPath, std::nullopt, bSource_, paths_, tokmapByPP,
            model_.GetMacroInvocations(), abTokMapA2B_, abTokMapB2A_, bToks_,
            bTokOff_, collapsedGap->leftEntry->e, collapsedGap->leftBEnd,
            collapsedGap->rightATok, observerTokens->firstATok,
            observerLineBegin, firstObserverBBegin);
    if (!sourcePrefix)
      continue;

    std::optional<LineObserverRightClosure> closure =
        extendLineObserverRightClosure(
            tuBytes, tuPath, bSource_, paths_, tokmapByPP, abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_, observerConstructEnd, observerBEnd,
            observerTokens->lastATok, observerTokens->lastBTok);
    if (!closure)
      continue;

    const uint64_t editBegin = collapsedGap->leftEntry->e;
    const uint64_t editEnd = closure->sourceEnd;
    if (editBegin >= editEnd)
      continue;
    if (editOverlapsExisting(editBegin, editEnd)) {
      continue;
    }

    std::string replacement = sourcePrefix->text;
    if (replacement.empty())
      continue;
    if (!stringutils::outAtBOL(replacement))
      continue;

    LineDirectiveLocation loc = lineControlProof_.LogicalLocationAtOwnerOffset(
        tuBytes, tuPath, std::nullopt, observerLineBegin);
    if (!loc.producerProven)
      continue;
    replacement += lineDirs_.FormatLineDirective(loc.lineNo, loc.fileSpelling);
    replacement += tuBytes.slice(observerLineBegin, observerConstructEnd).str();
    replacement += closure->sourceSpelledExtension;

    TextEdit edit{editBegin,    editEnd, replacement, std::nullopt,
                  std::nullopt, {},      {},          {}};
    textEditAssembler_.CertifyTextEditMaterializedBByteRange(
        edit, collapsedGap->leftBEnd, firstObserverBBegin);
    textEditAssembler_.AttachAcceptedResultCarrier(
        edit, proofLattice_.AcceptedCandidateBuilder()
                  .BuildAcceptedTUTextEditCandidate(
                      AcceptedPathKind::TUByteSpanConservativeEdit, editBegin,
                      editEnd, replacement));
    tuEdits.push_back(std::move(edit));
  }

  return true;
}

bool RefoldLineObserverLayout::AppendIncludeRealizationEdits(
    DenseMap<uint64_t, IncludeEdits> &perInclude) const {
  if (abTokMapA2B_.empty() || abTokMapB2A_.empty())
    return true;

  const auto &tokmapByPP = model_.GetTokmapByPP();
  auto patchOverlapsExisting = [&](uint64_t includeId, uint64_t begin,
                                   uint64_t end) {
    auto it = perInclude.find(includeId);
    if (it == perInclude.end())
      return false;
    for (const IncludePatch &patch : it->second.patches) {
      if (begin < patch.aEnd && patch.aStart < end)
        return true;
    }
    return false;
  };

  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        lineControlProof_.LineStateObservableMacroSite(macro);
    if (!site || !site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!macro.cover.IsValid())
      continue;

    const RefoldModel::IncludeItem *include =
        model_.GetIncludeById(*site->ownerIncludeId);
    if (!include)
      continue;

    const std::string ownerFile = refoldIncludeEnteredFileSpelling(*include);
    if (!paths_.PathsEqual(*site->invFile, ownerFile))
      continue;

    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerFile));
    if (!bufOrErr) {
      continue;
    }
    const MemoryBuffer &mb = **bufOrErr;
    StringRef headerBytes = mb.getBuffer();
    if (*site->invB >= headerBytes.size())
      continue;

    // Only handle the fully token-preserved case. If the builtin expansion is
    // already token-changed, ordinary macro/include materialization owns it.
    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [sourceLineBegin, sourceLineEnd] =
        lineObserverConstructSpan(headerBytes, *site);
    if (sourceLineBegin >= sourceLineEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(ownerFile, sourceLineBegin, sourceLineEnd,
                                    macro, paths_, tokmapByPP, abTokMapA2B_,
                                    aToks_, bToks_, bTokOff_,
                                    /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    std::optional<LineObserverOwnLineMerge> ownLineMerge =
        resolveLineObserverOwnLineMerge(
            headerBytes, ownerFile, bSource_, paths_, tokmapByPP, abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_, sourceLineBegin, sourceLineEnd,
            *observerTokens);
    if (!ownLineMerge)
      continue;

    const RefoldModel::TokMapEntry &prevEntry = *ownLineMerge->prevEntry;
    const uint64_t previousATok = ownLineMerge->previousATok;
    const size_t prevBTok = ownLineMerge->prevBTok;
    const uint64_t prevBEnd = ownLineMerge->prevBEnd;
    const uint64_t initialBEnd = ownLineMerge->initialBEnd;
    const LineObserverRightClosure &ownLineClosure = ownLineMerge->closure;

    // Include patches operate in PP-token ranges. Use the previous token as the
    // left edge so the materialized replacement owns the removed inter-line
    // separator without requiring a half-token byte edit in the header planner.
    const uint64_t patchAStart = previousATok;
    const uint64_t patchAEnd = ownLineClosure.lastATok + 1;
    const uint64_t patchBStart = static_cast<uint64_t>(prevBTok);
    const uint64_t patchBEnd =
        static_cast<uint64_t>(ownLineClosure.lastBTok + 1);
    if (patchOverlapsExisting(include->id, patchAStart, patchAEnd)) {
      continue;
    }

    // As in the TU theorem, the left/observer portion is a B materialization,
    // but any right-extension lines are preserved from the original header
    // spelling.  This lets an adjacent macro call stay source-spelled while
    // still closing the full B physical line around the observer.
    std::string replacement = bSource_.slice(prevBEnd, initialBEnd).str();
    replacement += ownLineClosure.sourceSpelledExtension;

    IncludePatch patch{include,     std::move(replacement),
                       patchAStart, patchAEnd,
                       patchBStart, patchBEnd};
    patch.hasDirectHeaderByteRange = true;
    patch.directHeaderByteBegin = prevEntry.e;
    patch.directHeaderByteEnd = ownLineClosure.sourceEnd;

    auto [it, _] = perInclude.try_emplace(include->id, include);
    it->second.Add(std::move(patch));
  }

  // Header-owned analogue of the TU pre-observer layout theorem.  When B
  // collapses an earlier ordinary source line into the prefix before a
  // preserved `__LINE__` observer, and the observer itself remains at BOL, the
  // header must materialize that collapsed prefix and then re-establish the
  // producer-proven logical state immediately before the observer.  This is
  // distinct from the own-line-merge theorem above: here a legal directive line
  // exists, so preserving the source-spelled observer is the stronger
  // refolding.
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    const RefoldModel::MacroInvocation *site =
        lineControlProof_.LineStateObservableMacroSite(macro);
    if (!site || !site->ownerIncludeId)
      continue;
    if (!site->invFile || !site->invB || !site->invE)
      continue;
    if (!macro.cover.IsValid())
      continue;

    const RefoldModel::IncludeItem *include =
        model_.GetIncludeById(*site->ownerIncludeId);
    if (!include)
      continue;

    const std::string ownerFile = refoldIncludeEnteredFileSpelling(*include);
    if (!paths_.PathsEqual(*site->invFile, ownerFile))
      continue;

    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(ownerFile));
    if (!bufOrErr) {
      continue;
    }
    const MemoryBuffer &mb = **bufOrErr;
    StringRef headerBytes = mb.getBuffer();
    if (*site->invB >= headerBytes.size())
      continue;

    if (!lineObserverCoverIsTokenPreserved(macro, abTokMapA2B_, abTokMapB2A_,
                                           aToks_, bToks_))
      continue;

    const auto [observerLineBegin, observerConstructEnd] =
        lineObserverConstructSpan(headerBytes, *site);
    if (observerLineBegin >= observerConstructEnd)
      continue;

    std::optional<LineObserverConstructTokens> observerTokens =
        lineObserverConstructTokens(
            ownerFile, observerLineBegin, observerConstructEnd, macro, paths_,
            tokmapByPP, abTokMapA2B_, aToks_, bToks_, bTokOff_,
            /*requirePreviousATok=*/true);
    if (!observerTokens)
      continue;

    const uint64_t firstObserverBBegin = observerTokens->firstBBegin;

    const std::optional<uint64_t> latestLineControlEnd =
        lineControlProof_.LatestProducerLineControlEndBefore(
            site->ownerIncludeId, ownerFile, observerLineBegin);
    const uint64_t searchBegin = latestLineControlEnd.value_or(0);

    const uint64_t observerBEnd = observerTokens->bEnd;

    std::optional<LineObserverCollapsedGap> collapsedGap =
        findLineObserverCollapsedPreGap(
            headerBytes, ownerFile, bSource_, searchBegin, observerLineBegin,
            observerTokens->firstATok, paths_, tokmapByPP, abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_);
    if (!collapsedGap)
      continue;

    const RefoldModel::TokMapEntry &leftEntry = *collapsedGap->leftEntry;
    if (firstObserverBBegin < collapsedGap->leftBEnd ||
        firstObserverBBegin > bSource_.size())
      continue;

    if (observerBEnd > bSource_.size())
      continue;

    std::optional<LineObserverSourcePrefix> sourcePrefix =
        buildLineObserverSourceSpelledPrefix(
            headerBytes, ownerFile, site->ownerIncludeId, bSource_, paths_,
            tokmapByPP, model_.GetMacroInvocations(), abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_, leftEntry.e, collapsedGap->leftBEnd,
            collapsedGap->rightATok, observerTokens->firstATok,
            observerLineBegin, firstObserverBBegin);
    if (!sourcePrefix)
      continue;

    std::optional<LineObserverRightClosure> closure =
        extendLineObserverRightClosure(
            headerBytes, ownerFile, bSource_, paths_, tokmapByPP, abTokMapA2B_,
            abTokMapB2A_, bToks_, bTokOff_, observerConstructEnd, observerBEnd,
            observerTokens->lastATok, observerTokens->lastBTok);
    if (!closure)
      continue;

    std::string replacement = sourcePrefix->text;
    if (replacement.empty())
      continue;
    if (!stringutils::outAtBOL(replacement))
      continue;

    LineDirectiveLocation loc = lineControlProof_.LogicalLocationAtOwnerOffset(
        headerBytes, ownerFile, site->ownerIncludeId, observerLineBegin);
    if (!loc.producerProven)
      continue;
    replacement += lineDirs_.FormatLineDirective(loc.lineNo, loc.fileSpelling);
    replacement +=
        headerBytes.slice(observerLineBegin, observerConstructEnd).str();
    replacement += closure->sourceSpelledExtension;

    const uint64_t patchAStart = collapsedGap->rightATok;
    const uint64_t patchAEnd = closure->lastATok + 1;
    const uint64_t patchBStart = static_cast<uint64_t>(collapsedGap->leftBTok);
    const uint64_t patchBEnd = static_cast<uint64_t>(closure->lastBTok + 1);
    if (patchOverlapsExisting(include->id, patchAStart, patchAEnd)) {
      continue;
    }

    IncludePatch patch{include,     std::move(replacement),
                       patchAStart, patchAEnd,
                       patchBStart, patchBEnd};
    patch.hasDirectHeaderByteRange = true;
    patch.directHeaderByteBegin = leftEntry.e;
    patch.directHeaderByteEnd = closure->sourceEnd;

    auto [it, _] = perInclude.try_emplace(include->id, include);
    it->second.Add(std::move(patch));
  }

  return true;
}

bool RefoldLineObserverLayout::LineResyncShouldDeferToConditionalJoin(
    StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
    uint64_t resumeOffset) const {
  std::optional<LineStateObserverSite> firstObserver =
      lineControlProof_.FirstOwnerSuffixLineStateObserverSite(
          ownerIncludeId, ownerFile, resumeOffset);
  if (!firstObserver || !firstObserver->demand.Any())
    return false;

  for (const RefoldModel::CondGroup *group :
       model_.GetCondGroups(ownerFile, ownerIncludeId)) {
    if (!group || group->file != ownerFile ||
        group->parentIncludeId != ownerIncludeId)
      continue;

    // The edit resumes inside a selected arm, but the first preserved
    // line-state observer is reached only after the conditional island has
    // rejoined.  A local arm repair is then only configuration-local; the
    // canonical domination point is the join repair before that observer.
    if (group->ContainsByte(resumeOffset) &&
        firstObserver->offset >= group->groupE)
      return true;
  }

  return false;
}

LineControlWrappedText
RefoldLineObserverLayout::WrapIncludeExpansionForMaterialization(
    const RefoldModel::IncludeItem &child, StringRef parentFileSpelling,
    StringRef parentOwnerFileForDemand,
    std::optional<uint64_t> parentOwnerIncludeId, uint64_t parentResumeOffset,
    size_t childEntryLineNo, size_t parentResumeLineNo, StringRef childBody,
    ArrayRef<FinalLineControlPruneCandidate> childBodyLineControlCandidates,
    ArrayRef<FinalLineControlSourceMapping> childBodyLineControlSourceMappings,
    bool allowUnobservableLineDirectiveSuppression) const {
  LineControlWrappedText wrapped;

  auto appendChildBody = [&]() {
    const uint64_t bodyBegin = static_cast<uint64_t>(wrapped.text.size());
    wrapped.text += childBody.str();
    appendShiftedLineControlPruneCandidates(wrapped.lineControlPruneCandidates,
                                            childBodyLineControlCandidates,
                                            bodyBegin);
    appendShiftedLineControlSourceMappings(wrapped.lineControlSourceMappings,
                                           childBodyLineControlSourceMappings,
                                           bodyBegin);
  };

  if (!lineDirs_.Enabled()) {
    appendChildBody();
    return wrapped;
  }

  const std::string childFileSpelling = refoldIncludeEnteredFileSpelling(child);

  const LineStateObserverDemand childDemand =
      lineControlProof_.IncludeSubtreeLineStateObserverDemand(child.id);
  const LineStateObserverDemand parentDemand =
      lineControlProof_.OwnerSuffixLineStateObserverDemand(
          parentOwnerIncludeId, parentOwnerFileForDemand, parentResumeOffset);
  const bool childNeedsLineState = childDemand.Any();
  const bool parentNeedsLineState = parentDemand.Any();
  const bool childNeedsLayoutBarrier =
      !childBody.empty() &&
      lineControlProof_.IncludeEntryLineDirectiveDischargesLayoutBarrier(
          child, parentOwnerFileForDemand);

  auto forEachLineControlDemandComponent =
      [&](const LineStateObserverDemand &demand, bool includeLayoutLineState,
          auto &&fn) {
        bool emittedAny = false;
        if (demand.needsLine) {
          fn(OwnerStateComponent::LineNumber);
          emittedAny = true;
        }
        if (demand.needsFile) {
          fn(OwnerStateComponent::FileState);
          emittedAny = true;
        }
        if (demand.needsFileName) {
          fn(OwnerStateComponent::FileName);
          emittedAny = true;
        }

        // Include-entry wrappers are sometimes required by a -E -P layout
        // barrier even before a preserved location builtin is known.  Keep that
        // obligation on the line-control gateway surface as a line-state
        // transition; the gateway will discharge it as suffix-unobserved if the
        // persistent observer graph confirms that no suffix owner observes it.
        if (!emittedAny && includeLayoutLineState)
          fn(OwnerStateComponent::LineNumber);
      };

  auto checkLineControlRepair =
      [&](const OwnerStateBoundary &boundary, OwnerStateComponent component,
          StateMutationKind mutation, StringRef stage, StringRef detail,
          bool requireKnownObserver) {
        return OwnerStateProof().CheckStateTransitionAcrossEditBoundary(
            boundary, component, mutation,
            OwnerStateProof().BuildStateTransitionWitness(
                SuffixStabilityWitnessKind::StateRepair, component, boundary,
                detail),
            stage, detail, requireKnownObserver);
      };

  auto parentReturnShouldDeferToConditionalJoin = [&]() -> bool {
    // If the include is materialized inside one arm of a preserved conditional
    // group and the first parent observer is after the group rejoins, an
    // arm-local return #line is insufficient for other build configurations.
    // The repair must dominate the join instead.  Do not evaluate the #if
    // expression here; this is a purely structural dominance proof over the
    // recorded conditional island and the first preserved observer site.
    return LineResyncShouldDeferToConditionalJoin(
        parentOwnerFileForDemand, parentOwnerIncludeId, parentResumeOffset);
  };

  auto appendSyntheticDirective =
      [&](std::string directive, FinalLineDirective::Origin origin,
          std::optional<FinalLineControlOwnerKey> owner,
          bool removableCandidate, FinalLineControlObligation obligation) {
        const uint64_t begin = static_cast<uint64_t>(wrapped.text.size());
        wrapped.text += directive;
        const uint64_t end = static_cast<uint64_t>(wrapped.text.size());
        if (removableCandidate) {
          wrapped.lineControlPruneCandidates.push_back(
              makeSyntheticLineControlPruneCandidate(
                  begin, end, origin, std::move(owner), obligation));
        }
      };

  auto appendChildEntryIfDemanded = [&]() {
    if (childNeedsLineState || childNeedsLayoutBarrier) {
      const OwnerStateBoundary childEntryBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              childFileSpelling, 0, 0, std::optional<uint64_t>(child.id)));
      forEachLineControlDemandComponent(
          childDemand, childNeedsLayoutBarrier,
          [&](OwnerStateComponent component) {
            const std::string detail =
                llvm::formatv("synthetic include-entry #line for inc#{0} "
                              "component={1} child='{2}'",
                              child.id, component, childFileSpelling)
                    .str();
            (void)checkLineControlRepair(
                childEntryBoundary, component, StateMutationKind::Replayed,
                "linedir/include-entry", detail,
                /*requireKnownObserver=*/childNeedsLineState);
          });
      appendSyntheticDirective(
          lineDirs_.FormatLineDirective(childEntryLineNo, childFileSpelling),
          FinalLineDirective::Origin::SyntheticIncludeEntry,
          FinalLineControlOwnerKey(childFileSpelling, child.id),
          /*removableCandidate=*/true,
          childNeedsLineState
              ? FinalLineControlObligation::SourceStateRepair
              : FinalLineControlObligation::LayoutBoundaryRepair);
      return;
    }
  };

  auto appendParentReturnIfDemanded = [&]() {
    if (parentNeedsLineState) {
      if (parentReturnShouldDeferToConditionalJoin()) {
        return;
      }
      const OwnerStateBoundary parentReturnBoundary =
          OwnerStateBoundary::FromSource(OwnerSourceRange::From(
              parentOwnerFileForDemand, parentResumeOffset, parentResumeOffset,
              parentOwnerIncludeId));
      forEachLineControlDemandComponent(
          parentDemand, /*includeLayoutLineState=*/false,
          [&](OwnerStateComponent component) {
            const std::string detail =
                llvm::formatv("synthetic include-return #line for inc#{0} "
                              "component={1} parent='{2}' byte={3}",
                              child.id, component, parentOwnerFileForDemand,
                              parentResumeOffset)
                    .str();
            (void)checkLineControlRepair(parentReturnBoundary, component,
                                         StateMutationKind::Replayed,
                                         "linedir/include-return", detail,
                                         /*requireKnownObserver=*/true);
          });
      appendSyntheticDirective(
          lineDirs_.FormatLineDirective(parentResumeLineNo, parentFileSpelling),
          FinalLineDirective::Origin::SyntheticIncludeReturn,
          FinalLineControlOwnerKey(parentOwnerFileForDemand.str(),
                                   parentOwnerIncludeId),
          /*removableCandidate=*/true,
          FinalLineControlObligation::IncludeReturnRepair);
      return;
    }
  };

  if (!allowUnobservableLineDirectiveSuppression) {
    // Ordinary include materialization and sideband-driven materialization use
    // the same proof obligation for synthetic line-control.  The local emission
    // policy remains conservative, but every synthetic wrapper emitted here is
    // registered as an explicit removable candidate; final observer/layout
    // liveness plus executable clang -E -P validation decide whether it
    // survives.
    wrapped.text.reserve(childBody.size() + 128);
    appendChildEntryIfDemanded();
    appendChildBody();
    if (!childBody.empty() && childBody.back() != '\n')
      wrapped.text += '\n';
    appendParentReturnIfDemanded();
    deduplicateLineControlPruneCandidates(wrapped.lineControlPruneCandidates);
    deduplicateLineControlSourceMappings(wrapped.lineControlSourceMappings);
    return wrapped;
  }

  if (childBody.empty() && !childNeedsLineState && !childNeedsLayoutBarrier &&
      !parentNeedsLineState) {
    appendChildBody();
    return wrapped;
  }

  wrapped.text.reserve(childBody.size() + 128);
  appendChildEntryIfDemanded();
  appendChildBody();

  if (!wrapped.text.empty() && wrapped.text.back() != '\n')
    wrapped.text += '\n';
  appendParentReturnIfDemanded();

  deduplicateLineControlPruneCandidates(wrapped.lineControlPruneCandidates);
  deduplicateLineControlSourceMappings(wrapped.lineControlSourceMappings);
  return wrapped;
}

} // namespace refold
} // namespace clang
