//===--- RefoldMacroReplay.cpp ----------------------------------*- C++ -*-===//
//
// Implements macro replay, actual-layout, whole-cover, paste-spelling, and
// boundary-selection helpers.  Keeping these adjacent macro primitives in one
// implementation file reduces file proliferation while preserving separate
// classes for occurrence replay, layout recovery, paste spelling, whole-cover
// proof, and boundary candidate selection.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroReplay.h"

#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroTopology.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

#include "clang/Lex/Lexer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <string>

using namespace llvm;
using namespace clang::refold;

namespace {

/// Reproduce Clang's `#` (stringize) operator on a macro-argument spelling.
///
/// Per C 6.10.3.2 / [cpp.stringize], white space between the argument's
/// preprocessing tokens collapses to a single space, leading and trailing white
/// space is dropped, and a `\` is inserted before each `\` and `"` that is part
/// of a string literal or character constant (including a string literal's
/// delimiting `"`).  Every other preprocessing token keeps its original
/// spelling, so a stray backslash is copied verbatim -- never doubled, unlike a
/// blanket C-string escape such as `quoteCString`.  The raw lexer classifies the
/// tokens (including prefixed and raw string literals), so the result matches
/// Clang exactly rather than approximating.
///
/// Returns the full string-literal spelling with surrounding quotes, or nullopt
/// when \p lang is unavailable (the caller then falls back to conservative
/// behavior and fails the replay closed).
std::optional<std::string>
stringizeMacroArgumentLikeClang(StringRef argText,
                                const clang::LangOptions *lang) {
  if (!lang)
    return std::nullopt;

  const clang::SourceLocation baseLoc =
      clang::SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = argText.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + argText.size();
  clang::Lexer lexer(baseLoc, *lang, bufStart, bufStart, bufEnd);

  std::string out = "\"";
  bool first = true;
  bool commentSpacePending = false;
  clang::Token tok;
  while (true) {
    lexer.LexFromRawLexer(tok);
    if (tok.is(clang::tok::eof))
      break;
    // A comment between tokens becomes a single inter-token space, exactly like
    // ordinary white space, before `#` is applied.
    if (tok.is(clang::tok::comment)) {
      commentSpacePending = true;
      continue;
    }
    if (!first && (tok.hasLeadingSpace() || commentSpacePending))
      out.push_back(' ');
    commentSpacePending = false;
    first = false;

    const unsigned offset =
        tok.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
    StringRef spelling(bufStart + offset, tok.getLength());

    const clang::tok::TokenKind kind = tok.getKind();
    const bool escapeAsLiteral =
        clang::tok::isStringLiteral(kind) ||
        kind == clang::tok::char_constant ||
        kind == clang::tok::wide_char_constant ||
        kind == clang::tok::utf8_char_constant ||
        kind == clang::tok::utf16_char_constant ||
        kind == clang::tok::utf32_char_constant;
    if (escapeAsLiteral) {
      // Escape only the `\` and `"` that belong to the literal's spelling.
      for (char c : spelling) {
        if (c == '\\' || c == '"')
          out.push_back('\\');
        out.push_back(c);
      }
    } else {
      out.append(spelling.begin(), spelling.end());
    }
  }
  out.push_back('"');
  return out;
}

} // namespace

//===----------------------------------------------------------------------===//
// RefoldMacroOccurrenceReplay
//===----------------------------------------------------------------------===//

RefoldMacroOccurrenceReplay::RefoldMacroOccurrenceReplay(Dependencies deps)
    : deps_(deps) {}

std::optional<std::pair<size_t, size_t>>
RefoldMacroOccurrenceReplay::GetOwnedPureInsertionBRangeForArgSpan(
    const RefoldModel::PPArgSpan &span,
    ArrayRef<RefoldModel::PPArgSpan> argSpans,
    std::pair<size_t, size_t> mappedEnv, const diffutils::Hunk &h) const {
  // This helper only applies to pure insertions with a non-empty B-side token
  // range.
  if (h.aStart != h.aEnd || h.bStart >= h.bEnd)
    return std::nullopt;

  const uint64_t aPos = h.aStart;
  const size_t insB0 = static_cast<size_t>(h.bStart);
  const size_t insB1 = static_cast<size_t>(h.bEnd);
  const size_t envB0 = mappedEnv.first;

  // If the gap lies inside the occurrence itself, the owned B range is just the
  // raw inserted token range.
  if (aPos >= span.begin && aPos < span.end)
    return std::make_pair(insB0, insB1);

  const bool isCommaSeparator =
      aPos < deps_.aToks.size() &&
      deps_.aToks[static_cast<size_t>(aPos)].spelling == ",";

  // Exact separator-before-right-occurrence case:
  //
  //   A:  ... , <span> ...
  //          ^
  //        aPos
  //
  // The raw inserted B range includes the shared leading separator. To make the
  // insertion occurrence-owned by the right-hand span, shift the owned B range
  // right by one token so that:
  //   - the shared leading comma is excluded, and
  //   - the comma that now precedes the original occurrence in B is included.
  //
  // The mapped envelope for the occurrence must begin exactly one token after
  // the raw inserted range; otherwise this structural ownership transform does
  // not hold.
  if (isCommaSeparator && span.begin == aPos + 1) {
    if (envB0 == insB1 + 1)
      return std::make_pair(insB0 + 1, insB1 + 1);
    return std::nullopt;
  }

  // Exact span-end ownership:
  //
  // If the gap is exactly at this occurrence's end, the raw inserted B range is
  // owned by this occurrence only when no right-hand occurrence begins at the
  // same A-side frontier. Token-diff pure insertions are anchored before token
  // `aPos`; when another occurrence begins at `aPos`, the insertion is a
  // prefix edit of that right-hand half-open span rather than a suffix edit of
  // this left-hand span. This prevents adjacent generated-callee/argument
  // occurrences from both claiming one insertion.
  if (aPos == span.end) {
    for (const auto &s : argSpans) {
      if (s.begin == aPos)
        return std::nullopt;
    }

    if (isCommaSeparator) {
      for (const auto &s : argSpans) {
        if (s.begin == aPos + 1)
          return std::nullopt;
      }
    }
    return std::make_pair(insB0, insB1);
  }

  // This occurrence does not exactly own the pure insertion.
  return std::nullopt;
}

bool RefoldMacroOccurrenceReplay::
    MacroArgReplacementMatchesAllOccurrencesInBImpl(
        const RefoldModel::MacroInvocation &m, uint32_t argIdx,
        StringRef baseArg, StringRef newArg,
        ArrayRef<diffutils::Hunk> tokenHunks, bool checkPasteSpans,
        OccurrenceSupportMode supportMode) const {
  if (newArg.data() == nullptr)
    return false;

  // Conservatism: if we cannot locate any occurrence metadata for this arg,
  // do not block args-only.
  bool hasAny = false;
  for (const auto &s : m.argSpans) {
    if (s.argIdx == argIdx) {
      hasAny = true;
      break;
    }
  }

  // Return true if the formal argument has direct producer evidence in this
  // invocation through an ordinary arg span, stringify span, or paste span.
  auto hasDirectOccurrenceSupport = [](const RefoldModel::MacroInvocation &inv,
                                       uint32_t formalIdx) -> bool {
    for (const auto &s : inv.argSpans) {
      if (s.argIdx == formalIdx)
        return true;
    }
    for (const auto &s : inv.stringifySpans) {
      if (s.argIdx == formalIdx)
        return true;
    }
    for (const auto &s : inv.pasteSpans) {
      if (s.argIdx == formalIdx)
        return true;
    }
    return false;
  };

  if (!hasAny) {
    if (supportMode == OccurrenceSupportMode::CurrentInvocationOnly) {
      if (!hasDirectOccurrenceSupport(m, argIdx))
        return false;
    } else {
      std::function<bool(const RefoldModel::MacroInvocation &, uint32_t,
                         std::set<std::pair<uint64_t, uint32_t>> &)>
          hasOccurrenceSupportThroughGraph;

      // Return true if this formal argument has occurrence evidence either
      // directly in `inv` or indirectly through the macro-expansion graph. The
      // search follows argument dependencies down into child invocations and
      // sideways into sibling invocations that consume the same caller argument
      // material.
      hasOccurrenceSupportThroughGraph =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t formalIdx,
              std::set<std::pair<uint64_t, uint32_t>> &visiting) -> bool {
        // Guard each `(invocation, formal)` state against cycles in the macro
        // graph. A cycle cannot provide a new finite occurrence witness by
        // itself.
        std::pair<uint64_t, uint32_t> key{inv.id, formalIdx};
        if (!visiting.insert(key).second)
          return false;

        auto eraseOnExit = llvm::make_scope_exit([&] { visiting.erase(key); });

        // Prefer concrete producer evidence on the current invocation: ordinary
        // argument spans, stringify spans, or paste spans.
        if (hasDirectOccurrenceSupport(inv, formalIdx))
          return true;

        // Search downward through child macro invocations. If a child formal
        // depends on this formal, then a direct/indirect occurrence of that
        // child formal also proves that this formal's material participates in
        // emitted output.
        ArrayRef<const RefoldModel::MacroInvocation *> children =
            deps_.macroTopology->MacroChildrenOf(inv.id);
        if (!children.empty()) {
          for (const auto *child : children) {
            for (uint32_t childFormalIdx = 0;
                 childFormalIdx < child->argDeps.size(); ++childFormalIdx) {
              bool dependsOnFormal = false;
              for (uint32_t dep : child->argDeps[childFormalIdx]) {
                if (dep == formalIdx) {
                  dependsOnFormal = true;
                  break;
                }
              }
              if (!dependsOnFormal)
                continue;

              if (hasOccurrenceSupportThroughGraph(*child, childFormalIdx,
                                                   visiting)) {
                return true;
              }
            }
          }
        }

        // Search sideways through sibling invocations under the same caller.
        // This handles wrapper shapes where the current invocation's formal is
        // forwarded through caller argument dependencies and the observable
        // occurrence appears in another child invocation of that caller.
        if (inv.callerMacroId && formalIdx < inv.argDeps.size()) {
          ArrayRef<const RefoldModel::MacroInvocation *> parentChildren =
              deps_.macroTopology->MacroChildrenOf(*inv.callerMacroId);
          if (!parentChildren.empty()) {
            ArrayRef<uint32_t> deps = inv.argDeps[formalIdx];

            for (const auto *sib : parentChildren) {
              if (sib->id == inv.id)
                continue;

              for (uint32_t sibFormalIdx = 0;
                   sibFormalIdx < sib->argDeps.size(); ++sibFormalIdx) {
                // A sibling formal is relevant only if it consumes at least one
                // of the same caller-level formal dependencies as the current
                // formal.
                bool sharesCallerDeps = false;
                for (uint32_t sibDep : sib->argDeps[sibFormalIdx]) {
                  if (llvm::is_contained(deps, sibDep)) {
                    sharesCallerDeps = true;
                    break;
                  }
                }
                if (!sharesCallerDeps)
                  continue;

                if (hasOccurrenceSupportThroughGraph(*sib, sibFormalIdx,
                                                     visiting)) {
                  return true;
                }
              }
            }
          }
        }

        // No direct occurrence, descendant occurrence, or sibling occurrence
        // proved that this formal participates in emitted output.
        return false;
      };

      std::set<std::pair<uint64_t, uint32_t>> visiting;
      if (!hasOccurrenceSupportThroughGraph(m, argIdx, visiting))
        return false;
    }
  }

  const uint64_t maxTok = deps_.bTokOff.empty()
                              ? 0ULL
                              : static_cast<uint64_t>(deps_.bTokOff.size() - 1);
  StringRef argTrim = newArg.trim();
  StringRef baseTrim = baseArg.trim();

  // If this arg is stringified anywhere, accept args-only without enforcing
  // paste-span checks.
  bool argIsStringified = false;
  if (deps_.strict) {
    for (const auto &s : m.stringifySpans) {
      if (s.argIdx == argIdx) {
        argIsStringified = true;
        break;
      }
    }

    // Check all STRINGIFY spans for this argument, but only in strict mode.
    if (argIsStringified) {
      auto canonArg = stringutils::canonicalizeStringifyInversePayload(argTrim);
      if (!canonArg || StringRef(*canonArg).trim() != argTrim)
        return false;

      for (const auto &s : m.stringifySpans) {
        if (s.argIdx != argIdx)
          continue;

        auto bEnv = deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(s);
        if (!bEnv)
          return false;

        // Extend the B-envelope to account for hunks that touch this
        // occurrence. This is required for insertions at the argument boundary
        // (e.g. appending tokens).
        if (!tokenHunks.empty()) {
          size_t lo = bEnv->first;
          size_t hi = bEnv->second;
          for (const auto &h : tokenHunks) {
            if (auto owned = GetOwnedPureInsertionBRangeForArgSpan(
                    s, m.stringifySpans, *bEnv, h)) {
              lo = std::min(lo, owned->first);
              hi = std::max(hi, owned->second);
              continue;
            }

            bool touches;
            if (h.aStart == h.aEnd) {
              touches = false;
            } else {
              // The normal case: split the rewritten core around the original
              // literal delimiters and require a unique segmentation.
              touches = (h.aStart < s.end && h.aEnd > s.begin);
            }
            if (touches && h.bStart < h.bEnd) {
              lo = static_cast<size_t>(std::min<uint64_t>(lo, h.bStart));
              hi = static_cast<size_t>(std::max<uint64_t>(hi, h.bEnd));
            }
          }
          lo = static_cast<size_t>(std::clamp<uint64_t>(lo, 0ULL, maxTok));
          hi = static_cast<size_t>(std::clamp<uint64_t>(hi, lo, maxTok));
          bEnv = {lo, hi};
        }

        StringRef tok =
            deps_.sourceMapper->SliceBSource(bEnv->first, bEnv->second).trim();
        if (tok.empty())
          return false;

        // If the occurrence records byte offsets inside the original A token,
        // reduce the rewritten B token to the corresponding editable core.
        // Prefer peeling the original prefix/suffix delimiters from the B
        // spelling; if the rewritten token no longer preserves those delimiters
        // verbatim, fall back to the same byte window clamped onto B. This
        // keeps the comparison focused on the argument payload rather than
        // surrounding literal text.
        if (s.byteBegin && s.byteEnd) {
          if ((bEnv->second - bEnv->first) != 1)
            return false;
          StringRef aTok = deps_.sourceMapper->SliceASource(
              static_cast<size_t>(s.begin), static_cast<size_t>(s.end));
          const uint64_t bb = *s.byteBegin;
          const uint64_t be = *s.byteEnd;
          if (be < bb || be > static_cast<uint64_t>(aTok.size()))
            return false;
          StringRef aPref = aTok.take_front(static_cast<size_t>(bb));
          StringRef aSuff = aTok.drop_front(static_cast<size_t>(be));
          if (tok.starts_with(aPref) && tok.ends_with(aSuff) &&
              tok.size() >= aPref.size() + aSuff.size()) {
            tok = tok.slice(aPref.size(), tok.size() - aSuff.size());
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            const uint64_t bbC = std::min<uint64_t>(bb, tok.size());
            const uint64_t beC = std::min<uint64_t>(be, tok.size());
            if (beC < bbC)
              return false;
            tok = tok.slice(static_cast<size_t>(bbC), static_cast<size_t>(beC));
          }
          tok = tok.trim();
        }

        // Compare against Clang's exact `#` stringization of the recovered
        // argument, not a blanket C-string escape.  `quoteCString` would double
        // a stray backslash (e.g. `a\tb` -> `"a\\tb"`), but `#(a\tb)` yields
        // `"a\tb"`, so the old model rejected valid stringified-argument folds
        // whenever the edited value contained an escape sequence.
        std::optional<std::string> expect =
            deps_.lexLang
                ? stringizeMacroArgumentLikeClang(argTrim, deps_.lexLang)
                : std::optional<std::string>(stringutils::quoteCString(argTrim));
        if (!expect || tok != *expect) {
          return false;
        }
      }
    }
  }

  // Determine whether token pasting consumes a prefix/suffix/whole segment of
  // this argument. 0=none/unknown, 1=prefix, 2=suffix, 3=whole, 4=ambiguous
  enum PasteType : unsigned { Unknown, Prefix, Suffix, Whole, Ambiguous };
  PasteType pasteConsume = Unknown;
  for (const auto &ps : m.pasteSpans) {
    if (ps.argIdx != argIdx)
      continue;

    StringRef aTokText =
        deps_.sourceMapper->SliceASource(ps.begin, ps.end).trim();
    if (aTokText.empty() || !ps.byteBegin || *ps.byteEnd < *ps.byteBegin ||
        static_cast<size_t>(*ps.byteEnd) > aTokText.size())
      continue;

    StringRef segA =
        aTokText.substr(*ps.byteBegin, *ps.byteEnd - *ps.byteBegin);
    if (segA.empty())
      continue;

    bool starts = baseTrim.starts_with(segA);
    bool ends = baseTrim.ends_with(segA);

    PasteType dir = Unknown;
    if (baseTrim == segA)
      dir = Whole;
    else if (starts && !ends)
      dir = Prefix;
    else if (ends && !starts)
      dir = Suffix;
    else if (starts && ends)
      dir = Ambiguous;
    else
      continue;

    if (pasteConsume == Unknown)
      pasteConsume = dir;
    else if (pasteConsume != dir)
      pasteConsume = Ambiguous;
  }

  // Verify all standard (non-paste) occurrences.
  for (const auto &s : m.argSpans) {
    if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
      continue;

    auto bEnv = deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(s);
    if (!bEnv)
      return false;

    // If the argument was deleted entirely in B, the mapped envelope may be
    // empty. Accept this only when the replacement is also empty after
    // trimming.
    if (bEnv->second < bEnv->first)
      return false;

    // Extend the B-envelope to account for hunks that touch this occurrence.
    // This is required for insertions at the argument boundary (e.g. appending
    // tokens).
    if (!tokenHunks.empty()) {
      size_t lo = bEnv->first;
      size_t hi = bEnv->second;
      for (const auto &h : tokenHunks) {
        if (auto owned = GetOwnedPureInsertionBRangeForArgSpan(s, m.argSpans,
                                                               *bEnv, h)) {
          lo = std::min(lo, owned->first);
          hi = std::max(hi, owned->second);
          continue;
        }

        bool touches;
        if (h.aStart == h.aEnd) {
          touches = false;
        } else {
          touches = (h.aStart < s.end && h.aEnd > s.begin);
        }
        if (touches && h.bStart < h.bEnd) {
          lo = static_cast<size_t>(std::min<uint64_t>(lo, h.bStart));
          hi = static_cast<size_t>(std::max<uint64_t>(hi, h.bEnd));
        }
      }
      lo = static_cast<size_t>(std::clamp<uint64_t>(lo, 0, maxTok));
      hi = static_cast<size_t>(std::clamp<uint64_t>(hi, lo, maxTok));
      bEnv = {lo, hi};
    }

    StringRef tokText =
        deps_.sourceMapper->SliceBSource(bEnv->first, bEnv->second).trim();
    if (tokText.empty()) {
      if (argTrim.empty())
        continue;
      return false;
    }

    bool ok;
    if (pasteConsume == Suffix) {
      // Suffix segment is consumed by pasting; standard expansion is the
      // prefix.
      ok = argTrim.starts_with(tokText);
    } else if (pasteConsume == Prefix) {
      // Prefix segment is consumed by pasting; standard expansion is the
      // suffix.
      ok = argTrim.ends_with(tokText);
    } else {
      ok = (tokText == argTrim);
    }

    if (!ok)
      return false;
  }

  // Paste-span verification is optional for callers that validate paste-token
  // correctness as a group (e.g., multi-span paste edits). When disabled, we
  // only validate standard+stringify occurrences above.
  if (argIsStringified || !checkPasteSpans)
    return true;

  // Verify all paste-span occurrences.
  for (const auto &ps : m.pasteSpans) {
    if (ps.argIdx != argIdx)
      continue;

    // This projected occurrence must still correspond to exactly one token in
    // B.
    auto bEnv = deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(ps);
    if (!bEnv || bEnv->second <= bEnv->first ||
        (bEnv->second - bEnv->first) != 1)
      return false;

    // Fetch the trimmed token text for this projected occurrence on both sides;
    // later checks will compare the corresponding projected subranges.
    StringRef aTokText =
        deps_.sourceMapper->SliceASource(ps.begin, ps.end).trim();
    if (aTokText.empty())
      return false;
    StringRef bTokText =
        deps_.sourceMapper->SliceBSource(bEnv->first, bEnv->second).trim();
    if (bTokText.empty())
      return false;

    // The projection's byte subrange must be valid within the A-side token.
    if (!ps.byteBegin || *ps.byteEnd < *ps.byteBegin ||
        static_cast<size_t>(*ps.byteEnd) > aTokText.size())
      return false;

    // Extract the original projected segment from the A-side token text.
    StringRef oldSeg =
        aTokText.substr(*ps.byteBegin, *ps.byteEnd - *ps.byteBegin);

    auto toSigned = [](std::optional<uint32_t> opt) -> int64_t {
      return static_cast<int64_t>(opt.value_or(0));
    };

    // Shift the projected byte range into B by the whole-token size delta.
    int64_t delta = static_cast<int64_t>(bTokText.size()) -
                    static_cast<int64_t>(aTokText.size());

    int64_t bb = toSigned(ps.byteBegin);
    int64_t be = toSigned(ps.byteEnd) + delta;

    // The translated byte range must remain valid within the B-side token.
    if (bb < 0 || be < bb || static_cast<uint64_t>(be) > bTokText.size())
      return false;

    uint64_t bbB = static_cast<uint64_t>(bb);
    uint64_t beB = static_cast<uint64_t>(be);

    // NOTE: Do NOT require the token outside this segment to be identical
    // between A and B. Multiple macro arguments can contribute to the same
    // pasted token, and a single edit hunk may simultaneously modify multiple
    // segments (e.g., a_b_c -> d_e_f). Full pasted-token consistency is
    // validated separately via pasteArgReplacementsMatchAllPasteTokensInB(...).

    StringRef segB = bTokText.substr(bbB, beB - bbB);

    // Determine where this pasted segment comes from within the original
    // argument spelling.
    bool starts = baseTrim.starts_with(oldSeg);
    bool ends = baseTrim.ends_with(oldSeg);

    // Accept either an exact projected replacement or the corresponding
    // prefix/suffix match when this segment represents a trimmed edge.
    bool ok = false;
    if (baseTrim == oldSeg)
      ok = (argTrim == segB);
    else if (starts && !ends)
      ok = argTrim.starts_with(segB);
    else if (ends && !starts)
      ok = argTrim.ends_with(segB);

    // Check to see if it's ambiguous or unclassified.
    if (!ok)
      return false;
  }

  return true;
}

//===----------------------------------------------------------------------===//
// RefoldMacroActualLayout
//===----------------------------------------------------------------------===//

RefoldMacroActualLayout::RefoldMacroActualLayout(Dependencies deps)
    : deps_(deps) {}

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldMacroActualLayout::GetMacroInvocationFormalArgContentRanges(
    const RefoldModel::MacroInvocation &m, StringRef invText) const {
  // Synthesize an empty argument range at the closing parenthesis. This is used
  // for omitted trailing variadic formals so callers still receive one range
  // per formal parameter.
  auto emptyAtCloseParenIn = [&](StringRef text) -> std::pair<size_t, size_t> {
    size_t closeIdx = text.rfind(')');
    if (closeIdx == StringRef::npos)
      closeIdx = text.size();
    return {closeIdx, closeIdx};
  };

  // A formal is variadic only if the producer recorded a matching formal
  // parameter entry and marked it variadic.
  auto isVariadicFormal = [&](size_t idx) -> bool {
    return idx < m.defParams.size() && m.defParams[idx].variadic;
  };

  // Missing actual arguments are accepted only for a suffix of variadic
  // formals. Non-variadic missing formals would make the invocation/formal
  // mapping ill-formed for refolding purposes.
  auto trailingFormalsAreVariadic = [&](size_t beginIdx) -> bool {
    for (size_t i = beginIdx; i < m.defParams.size(); ++i) {
      if (!isVariadicFormal(i))
        return false;
    }
    return true;
  };

  auto mapParsedActualsToFormalRanges =
      [&](StringRef text, const std::vector<std::pair<size_t, size_t>> &parsed)
      -> std::optional<std::vector<std::pair<size_t, size_t>>> {
    const size_t formalN = m.defParams.size();
    const size_t actualN = parsed.size();

    // Object-like macros have no formal argument ranges. If parsing found
    // actuals anyway, the spelling is not compatible with this macro
    // definition.
    if (formalN == 0) {
      if (actualN == 0)
        return std::vector<std::pair<size_t, size_t>>();
      return std::nullopt;
    }

    // The ordinary case: one parsed actual per formal.
    if (actualN == formalN)
      return parsed;

    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(formalN);

    // Too many actuals can only be represented when the final formal is
    // variadic. Collapse all surplus actuals into that final formal's range.
    if (actualN > formalN) {
      if (!isVariadicFormal(formalN - 1))
        return std::nullopt;
      out.insert(out.end(), parsed.begin(), parsed.begin() + (formalN - 1));
      out.push_back({parsed[formalN - 1].first, parsed.back().second});
      return out;
    }

    // Too few actuals are allowed only when every missing formal is variadic.
    // Represent omitted variadic actuals as empty ranges at the invocation's
    // closing parenthesis.
    if (!trailingFormalsAreVariadic(actualN))
      return std::nullopt;

    out.insert(out.end(), parsed.begin(), parsed.end());
    for (size_t i = actualN; i < formalN; ++i)
      out.push_back(emptyAtCloseParenIn(text));
    return out;
  };

  auto parseAndMapFormalRanges = [&](StringRef text)
      -> std::optional<std::vector<std::pair<size_t, size_t>>> {
    // Parse actual argument content ranges syntactically, then normalize the
    // actual list into one range per formal parameter.
    auto parsedOpt =
        RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
            text, *deps_.lexLang);
    if (!parsedOpt)
      return std::nullopt;
    return mapParsedActualsToFormalRanges(text, *parsedOpt);
  };

  auto tryNormalizedInvocationRanges =
      [&]() -> std::optional<std::vector<std::pair<size_t, size_t>>> {
    if (!m.normalizedInvText || invText != *m.normalizedInvText ||
        m.normalizedInvArgTextRanges.empty())
      return std::nullopt;

    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(m.normalizedInvArgTextRanges.size());
    for (const auto &r : m.normalizedInvArgTextRanges) {
      if (!r.first || !r.second || *r.second < *r.first ||
          *r.second > invText.size())
        return std::nullopt;
      out.emplace_back(static_cast<size_t>(*r.first),
                       static_cast<size_t>(*r.second));
    }
    return out;
  };

  auto tryProducerRelativeRanges =
      [&]() -> std::optional<std::vector<std::pair<size_t, size_t>>> {
    // Producer ranges are absolute source offsets. They are usable here only if
    // we know the invocation's absolute begin offset so they can be made
    // relative to `invText`.
    if (m.invArgRanges.empty() || !m.invB)
      return std::nullopt;

    const uint64_t invB = *m.invB;
    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(m.invArgRanges.size());

    for (const auto &r : m.invArgRanges) {
      // Every formal range must be complete and ordered.
      if (!r.first || !r.second)
        return std::nullopt;
      if (*r.first < invB || *r.second < *r.first)
        return std::nullopt;

      // Convert absolute offsets to offsets relative to the invocation text
      // being examined.
      const uint64_t relB64 = *r.first - invB;
      const uint64_t relE64 = *r.second - invB;
      if (relE64 > invText.size() || relB64 > relE64)
        return std::nullopt;

      out.emplace_back(static_cast<size_t>(relB64),
                       static_cast<size_t>(relE64));
    }

    return out;
  };

  auto transportArgsOverProducerSlotsExactly =
      [&](StringRef producerText,
          const std::vector<std::pair<size_t, size_t>> &producerRanges,
          StringRef currentText,
          const std::vector<std::pair<size_t, size_t>> &currentRanges) -> bool {
    // The producer and current invocation spellings must expose the same formal
    // slot structure before producer ranges can be trusted for the current
    // text.
    if (producerRanges.size() != currentRanges.size())
      return false;

    std::string rebuilt;
    rebuilt.reserve(currentText.size());
    size_t cur = 0;

    for (size_t i = 0; i < producerRanges.size(); ++i) {
      size_t pb = producerRanges[i].first;
      size_t pe = producerRanges[i].second;
      size_t cb = currentRanges[i].first;
      size_t ce = currentRanges[i].second;

      // Producer ranges must be ordered, non-overlapping slices of the producer
      // invocation spelling. Current ranges must be valid slices of the current
      // invocation spelling.
      if (pb > pe || pe > producerText.size() || pb < cur)
        return false;
      if (cb > ce || ce > currentText.size())
        return false;

      // Rebuild the current invocation by taking fixed text from the producer
      // spelling and argument slot payloads from the current spelling. If the
      // result equals `currentText`, then only argument contents changed and
      // the slot boundaries transported exactly.
      rebuilt.append(producerText.substr(cur, pb - cur));
      rebuilt.append(currentText.substr(cb, ce - cb));
      cur = pe;
    }

    rebuilt.append(producerText.substr(cur));
    return rebuilt == currentText;
  };

  if (auto normalizedRanges = tryNormalizedInvocationRanges())
    return normalizedRanges;

  if (m.invText) {
    // Prefer producer-provided ranges for the original invocation spelling.
    // If they are unavailable or invalid, fall back to syntactic parsing.
    std::optional<std::vector<std::pair<size_t, size_t>>> producerRangesOpt =
        tryProducerRelativeRanges();
    if (!producerRangesOpt)
      producerRangesOpt = parseAndMapFormalRanges(*m.invText);

    // If the requested text is the producer's original invocation spelling, the
    // original formal ranges are already the desired answer.
    if (invText == *m.invText)
      return producerRangesOpt;

    // For a rewritten invocation spelling, parse the current text and then
    // prove that the producer's fixed text plus the current argument slots
    // exactly reconstructs the current spelling.
    auto currentRangesOpt = parseAndMapFormalRanges(invText);
    if (!producerRangesOpt || !currentRangesOpt)
      return std::nullopt;

    if (!transportArgsOverProducerSlotsExactly(*m.invText, *producerRangesOpt,
                                               invText, *currentRangesOpt)) {
      return std::nullopt;
    }

    // The current text has the same slot structure as the producer text, so the
    // parsed current ranges are safe to return.
    return currentRangesOpt;
  }

  // No producer invocation spelling is available, so rely entirely on local
  // syntactic parsing of the supplied invocation text.
  return parseAndMapFormalRanges(invText);
}

//===----------------------------------------------------------------------===//
// RefoldMacroWholeCoverProof
//===----------------------------------------------------------------------===//

std::optional<std::pair<uint64_t, uint64_t>>
RefoldMacroWholeCoverProof::GetWholeCoverATokRange(
    const RefoldModel::MacroInvocation &m) {
  uint64_t covLoA = m.cover.begin;
  uint64_t covHiA = m.cover.end;

  // For function-like macros with no formal parameters, the producer may
  // conservatively widen the macro cover to include surrounding context (e.g.
  // when the invocation occurs in a nested macro argument). In these cases,
  // bodySpans is the precise expansion slice we want to whole-cover replace.
  if (m.subkind == "func" && m.defParams.empty() && !m.bodySpans.empty()) {
    uint64_t lo = std::numeric_limits<uint64_t>::max();
    uint64_t hi = 0;
    for (const auto &s : m.bodySpans) {
      if (s.begin < s.end) {
        lo = std::min(lo, s.begin);
        hi = std::max(hi, s.end);
      }
    }
    if (lo != std::numeric_limits<uint64_t>::max() && lo < hi) {
      covLoA = lo;
      covHiA = hi;
    }
  }

  if (covLoA >= covHiA)
    return std::nullopt;
  return std::make_pair(covLoA, covHiA);
}

bool RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained(
    const RefoldModel::MacroInvocation &m) {
  auto range = GetWholeCoverATokRange(m);
  if (!range)
    return false;
  const uint64_t covLoA = range->first;
  const uint64_t covHiA = range->second;

  SmallVector<std::pair<uint64_t, uint64_t>, 16> spans;

  // Clip every producer-recorded macro span to the whole-cover A-token range.
  // The self-contained proof only cares about whether the cover is completely
  // explained by spans owned by this invocation.
  auto appendIntersecting = [&](auto &&src) {
    for (const auto &sp : src) {
      uint64_t b = std::max<uint64_t>(covLoA, sp.begin);
      uint64_t e = std::min<uint64_t>(covHiA, sp.end);
      if (b < e)
        spans.emplace_back(b, e);
    }
  };

  appendIntersecting(m.bodySpans);
  appendIntersecting(m.argSpans);
  appendIntersecting(m.stringifySpans);
  appendIntersecting(m.pasteSpans);

  // No owned spans means the whole-cover range cannot be justified as an
  // invocation-local replacement domain.
  if (spans.empty())
    return false;

  llvm::sort(spans, [](const auto &a, const auto &b) {
    if (a.first != b.first)
      return a.first < b.first;
    return a.second < b.second;
  });

  // Sweep the clipped spans and require continuous coverage of [covLoA,
  // covHiA). Any uncovered token gap means the whole-cover rewrite would absorb
  // material not accounted for by this macro invocation's recorded provenance.
  uint64_t cur = covLoA;
  for (const auto &sp : spans) {
    if (sp.second <= cur)
      continue;
    if (sp.first > cur)
      return false;
    cur = std::max(cur, sp.second);
    if (cur >= covHiA)
      return true;
  }

  return cur >= covHiA;
}

//===----------------------------------------------------------------------===//
// RefoldMacroPasteSpelling
//===----------------------------------------------------------------------===//

std::string RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
    StringRef baseArg, StringRef oldSeg, StringRef newSeg) {
  StringRef baseTrim = baseArg.trim();
  StringRef oldTrim = oldSeg.trim();
  StringRef newTrim = newSeg.trim();

  // Idempotence: later hunks may refer to the same token-paste occurrence
  // after we've already applied a previous arg edit (e.g., a standard arg
  // occurrence). In that case, 'baseTrim' already equals the target segment
  // and we should treat this splice as a no-op rather than a failure.
  if (baseTrim == newTrim) {
    return baseTrim.str();
  }

  if (oldTrim.empty())
    return ""; // Return empty to signal failure/null

  // Only allow unambiguous boundary splices: whole arg, prefix, or suffix.
  if (baseTrim == oldTrim)
    return newTrim.str();

  bool starts = baseTrim.starts_with(oldTrim);
  bool ends = baseTrim.ends_with(oldTrim);

  // Ambiguous case: if it matches both as prefix and suffix, we can't safely
  // determine which occurrence to replace.
  if (starts && ends)
    return "";

  if (starts) {
    // Return new replacement + remaining suffix of the original arg.
    return (newTrim.str() + baseTrim.substr(oldTrim.size()).str());
  }

  if (ends) {
    // Return original prefix + new replacement.
    size_t prefixLen = baseTrim.size() - oldTrim.size();
    return (baseTrim.substr(0, prefixLen).str() + newTrim.str());
  }

  return "";
}

std::string RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArgExact(
    StringRef baseArg, uint32_t argByteBegin, uint32_t argByteEnd,
    StringRef oldSeg, StringRef newSeg) {
  if (argByteEnd < argByteBegin || argByteEnd > baseArg.size())
    return "";

  // The producer-provided range is authoritative only if it still names the
  // exact old segment inside the current invocation argument spelling. If it no
  // longer matches, falling back to prefix/suffix inference would reintroduce
  // the ambiguity this metadata is meant to avoid.
  if (baseArg.slice(argByteBegin, argByteEnd) != oldSeg)
    return "";

  return stringutils::replaceRange(baseArg, argByteBegin, argByteEnd, newSeg);
}

//===----------------------------------------------------------------------===//
// RefoldMacroBoundarySelector
//===----------------------------------------------------------------------===//

RefoldMacroBoundarySelector::RefoldMacroBoundarySelector(
    const RefoldModel &model, const RefoldMacroTopology &macroTopology,
    const RefoldSourceMapper &sourceMapper, ArrayRef<PPTok> bToks,
    const clang::LangOptions &lexLang)
    : model_(model), macroTopology_(macroTopology), sourceMapper_(sourceMapper),
      bToks_(bToks), lexLang_(lexLang) {}

const RefoldModel::MacroInvocation *
RefoldMacroBoundarySelector::RightBoundaryVaOptActivationMacro(
    uint64_t aGap, std::optional<uint64_t> ownerIncludeId) const {
  const RefoldModel::MacroInvocation *best = nullptr;
  uint64_t bestLen = std::numeric_limits<uint64_t>::max();

  auto definitionFor = [&](const RefoldModel::MacroInvocation &inv)
      -> const RefoldModel::MacroDirective * {
    if (!inv.definitionDirectiveId)
      return nullptr;
    return model_.GetMacroDirectiveById(*inv.definitionDirectiveId);
  };

  auto definitionContainsVaOpt =
      [](const RefoldModel::MacroDirective &directive) -> bool {
    for (const auto &tok : directive.replacementTokens) {
      if (tok.spelling == "__VA_OPT__")
        return true;
    }
    return false;
  };

  auto isDescendantOf = [&](const RefoldModel::MacroInvocation &child,
                            const RefoldModel::MacroInvocation &root) {
    const RefoldModel::MacroInvocation *cur = &child;
    for (size_t depth = 0; cur && depth <= model_.GetMacroInvocations().size();
         ++depth) {
      if (cur->id == root.id)
        return true;
      if (!cur->callerMacroId)
        return false;
      cur = macroTopology_.FindMacroInvocationById(*cur->callerMacroId);
    }
    return false;
  };

  auto hasVaOptDescendantAtBoundary =
      [&](const RefoldModel::MacroInvocation &root) {
        for (const RefoldModel::MacroInvocation &candidate :
             model_.GetMacroInvocations()) {
          if (!isDescendantOf(candidate, root))
            continue;
          const RefoldModel::MacroDirective *definition =
              definitionFor(candidate);
          if (definition && definitionContainsVaOpt(*definition))
            return true;
        }
        return false;
      };

  for (const RefoldModel::MacroInvocation &m : model_.GetMacroInvocations()) {
    if (ownerIncludeId) {
      if (!m.ownerIncludeId || *m.ownerIncludeId != *ownerIncludeId)
        continue;
    }

    if (!m.cover.IsValid() || m.cover.end <= m.cover.begin ||
        m.cover.end != aGap)
      continue;

    // Only real source callsites can own the eventual patch. Generated child
    // invocations may supply the `__VA_OPT__` evidence, but the emitted rewrite
    // must be applied to an invocation spelling that exists in source.
    if (!m.invB || !m.invE || !m.invText)
      continue;
    if (macroTopology_.IsInvocationInsideDefineDirective(m))
      continue;

    if (!hasVaOptDescendantAtBoundary(m))
      continue;

    const uint64_t len = m.cover.end - m.cover.begin;

    if (!best || len < bestLen || (len == bestLen && m.id < best->id)) {
      best = &m;
      bestLen = len;
    }
  }

  return best;
}

const RefoldModel::MacroInvocation *
RefoldMacroBoundarySelector::BoundaryGeneratedSelectorMacro(
    uint64_t aGap, std::optional<uint64_t> ownerIncludeId) const {
  const RefoldModel::MacroInvocation *best = nullptr;
  uint64_t bestLen = std::numeric_limits<uint64_t>::max();

  auto isDescendantOf = [&](const RefoldModel::MacroInvocation &child,
                            const RefoldModel::MacroInvocation &root) {
    const RefoldModel::MacroInvocation *cur = &child;
    for (size_t depth = 0; cur && depth <= model_.GetMacroInvocations().size();
         ++depth) {
      if (cur->id == root.id)
        return true;
      if (!cur->callerMacroId)
        return false;
      cur = macroTopology_.FindMacroInvocationById(*cur->callerMacroId);
    }
    return false;
  };

  auto hasGeneratedSelectorDescendant =
      [&](const RefoldModel::MacroInvocation &root) {
        for (const RefoldModel::MacroInvocation &candidate :
             model_.GetMacroInvocations()) {
          if (!isDescendantOf(candidate, root))
            continue;
          if (candidate.calleeOrigin.kind ==
                  MacroCalleeOriginKind::CallerParam &&
              !candidate.calleeOrigin.callerParamIndices.empty())
            return true;
        }
        return false;
      };

  for (const RefoldModel::MacroInvocation &m : model_.GetMacroInvocations()) {
    if (ownerIncludeId) {
      if (!m.ownerIncludeId || *m.ownerIncludeId != *ownerIncludeId)
        continue;
    }
    if (!m.cover.IsValid() || m.cover.end <= m.cover.begin)
      continue;
    if (m.cover.begin != aGap && m.cover.end != aGap)
      continue;
    if (!m.invB || !m.invE || !m.invText)
      continue;
    if (macroTopology_.IsInvocationInsideDefineDirective(m))
      continue;

    // Boundary insertions around a generated selector replacement are safe to
    // leave for the macro proof only when a descendant callee token actually
    // came from a caller parameter.  This keeps ordinary expression/include
    // boundary insertions out of macro ownership while allowing proofs such as
    // `STR(x)` -> `WRAP(x)`, where the added string-literal context appears on
    // both sides of the old generated callee expansion.
    if (!hasGeneratedSelectorDescendant(m))
      continue;

    const uint64_t len = m.cover.end - m.cover.begin;
    if (!best || len < bestLen || (len == bestLen && m.id < best->id)) {
      best = &m;
      bestLen = len;
    }
  }

  return best;
}

const RefoldModel::MacroInvocation *
RefoldMacroBoundarySelector::BoundaryDefinitionTapeReplayMacro(
    const diffutils::Hunk &hunk, std::optional<uint64_t> ownerIncludeId) const {
  // A pure insertion immediately before or after a macro expansion can still be
  // macro-owned when the defining replacement-list tape contains a zero-token
  // proof surface at that boundary.  This selector only exposes such boundary
  // owners to the definition-tape replay validator; it does not itself prove or
  // construct the repair.
  if (!hunk.isInsertOnly() || hunk.bStart >= hunk.bEnd)
    return nullptr;

  const RefoldModel::MacroInvocation *best = nullptr;
  uint64_t bestLen = std::numeric_limits<uint64_t>::max();

  auto definitionFor = [&](const RefoldModel::MacroInvocation &macro)
      -> const RefoldModel::MacroDirective * {
    if (!macro.definitionDirectiveId)
      return nullptr;
    return model_.GetMacroDirectiveById(*macro.definitionDirectiveId);
  };

  RefoldMacroActualLayout actualLayout({&lexLang_});
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (ownerIncludeId) {
      if (!macro.ownerIncludeId || *macro.ownerIncludeId != *ownerIncludeId)
        continue;
    }

    if (macro.subkind != "func" || !macro.cover.IsValid() ||
        macro.cover.end <= macro.cover.begin)
      continue;

    const bool atRecordedCoverBoundary =
        hunk.aStart == macro.cover.begin || hunk.aStart == macro.cover.end;
    // Also admit the one-token-left frontier when producer PP-byte expansion
    // bounds are available.  Repeated fixed literals at the start of a macro
    // replacement list can make token LCS slide an inserted actual token just
    // before the recorded token cover, even though the byte-level macro
    // expansion still starts at the first replacement-list token.  The actual
    // proof is discharged later by definition-tape replay against that recorded
    // PP-byte envelope; this selector only exposes the candidate owner so the
    // insertion is not prematurely emitted as a standalone TU/include edit.
    const bool immediatelyBeforeRecordedCover =
        macro.cover.begin > 0 && hunk.aStart + 1 == macro.cover.begin &&
        macro.invPPByteBegin && macro.invPPByteEnd;
    if (!atRecordedCoverBoundary && !immediatelyBeforeRecordedCover)
      continue;
    if (!macro.invB || !macro.invE || !macro.invText)
      continue;
    if (macroTopology_.IsInvocationInsideDefineDirective(macro))
      continue;
    if (macro.calleeOrigin.kind != MacroCalleeOriginKind::LiteralMacroName)
      continue;
    if (!macro.stringifySpans.empty() || !macro.pasteSpans.empty())
      continue;

    const RefoldModel::MacroDirective *definition = definitionFor(macro);
    if (!definition || definition->subkind != "#define" ||
        !definition->functionLike || definition->name != macro.name ||
        definition->defParams.size() != macro.defParams.size() ||
        definition->replacementTokens.empty())
      continue;

    auto bEnv =
        sourceMapper_.MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
            macro.cover.begin, macro.cover.end);
    if (!bEnv || bEnv->first > bEnv->second || bEnv->second > bToks_.size())
      continue;

    // Ordinary empty-slot boundary repairs are real B-token insertions at the
    // recorded macro-cover frontier, so the inserted B range must lie inside
    // the cover envelope.  The one-token-left case is an LCS placement
    // artifact: the replay solver must still prove a concrete invocation
    // rewrite before the candidate can absorb the hunk.
    if (!immediatelyBeforeRecordedCover &&
        (hunk.bStart < static_cast<uint64_t>(bEnv->first) ||
         hunk.bEnd > static_cast<uint64_t>(bEnv->second)))
      continue;

    auto rangesOpt = actualLayout.GetMacroInvocationFormalArgContentRanges(
        macro, *macro.invText);
    if (!rangesOpt)
      continue;

    bool hasEmptyFormalSourceSlot = false;
    for (const auto &range : *rangesOpt) {
      if (range.second < range.first || range.second > macro.invText->size())
        continue;
      if (StringRef(*macro.invText)
              .slice(range.first, range.second)
              .trim()
              .empty()) {
        hasEmptyFormalSourceSlot = true;
        break;
      }
    }

    bool hasMissingExpansionFormal = false;
    for (size_t formalIdx = 0; formalIdx < rangesOpt->size(); ++formalIdx) {
      bool sawTokenBearingStandardSpan = false;
      for (const auto &as : macro.argSpans) {
        if (as.kind == PPArgSpanKind::Standard && as.argIdx == formalIdx &&
            as.begin < as.end) {
          sawTokenBearingStandardSpan = true;
          break;
        }
      }
      if (!sawTokenBearingStandardSpan) {
        hasMissingExpansionFormal = true;
        break;
      }
    }

    if (!hasEmptyFormalSourceSlot && !hasMissingExpansionFormal &&
        !immediatelyBeforeRecordedCover)
      continue;

    const uint64_t len = macro.cover.end - macro.cover.begin;
    if (!best || len < bestLen || (len == bestLen && macro.id < best->id)) {
      best = &macro;
      bestLen = len;
    }
  }

  return best;
}
