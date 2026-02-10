//===--- RefoldEngine.cpp ---------------------------------------*- C++ -*-===//
//
// This component implements the deterministic “refolding” engine that projects
// edits made to a raw preprocessed stream (B) back onto the original, partially
// expanded translation unit (TU) described by the refold map.
//
// Overview
// --------
// RefoldEngine consumes:
//   • A: original preprocessed bytes and tokens
//   • B: edited preprocessed bytes and tokens
//   • M: RefoldModel (parsed from the JSON refold map)
//
// It aligns A↔B token streams, derives edit hunks, classifies each hunk as
// TU-owned / include-owned / macro-invocation–owned, and materializes a new
// TU that incorporates edits while preserving original structure and semantics.
//
// Responsibilities
// ----------------
//   • Compute LCS-based A→B anchors and contiguous edit hunks.
//   • Attribute hunks to includes or macro call sites using M’s coverage data.
//   • Normalize and coalesce include insertions (line-local, boundary safe).
//   • Realize include expansions bottom-up, applying macro patches in-owner.
//   • Apply TU-level replacements with boundary hygiene (no token gluing).
//
// Determinism & Policy
// --------------------
//   • All iteration and sorting are stable; edits apply high→low to avoid
//   drift. • Boundary padding inserts at most one space locally when needed by
//     maximal-munch rules; internal whitespace is preserved verbatim.
//   • Errors are reported via the Logging subsystem (`fatal()/error()/...`).
//
// Public Surface
// --------------
//   • std::string Refold(...): orchestrates the end-to-end refolding and
//     returns the refolded TU text.
//   • Helper utilities: token/byte mapping, hunk builders, include realization,
//     macro-patch construction, and line-local boundary checks.
//
// Notes
// -----
//   • No RTTI or exceptions required; mirrors LLVM/Clang style.
//   • Paths are compared via canonicalization (see RefoldEngine::PathsEqual()).
//   • All indices are half-open where applicable: tokens [lo,hi), bytes [b,e).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "RefoldLog.h"
#include "RefoldEngine.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <tuple>

using namespace llvm;

namespace clang {
namespace refold {

namespace {
inline std::string resolveHeaderPath(const RefoldModel::IncludeItem &inc) {
  return (inc.resolvedPath && !inc.resolvedPath->empty())
             ? inc.resolvedPath->str()
             : stringutils::stripHeaderToken(inc.target).str();
}

Error compareTokens(ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks) {
  // Let's first at least compare the tokens up to the min length to see if we
  // at least have a match prefix. We can complain about the length mismatch
  // later.
  const size_t n = std::min(aToks.size(), bToks.size());
  for (size_t i = 0; i < n; ++i) {
    const std::string aDbg = stringutils::showWS(
        stringutils::clip(StringRef(aToks[i].spelling), 100));
    const std::string bDbg = stringutils::showWS(
        stringutils::clip(StringRef(bToks[i].spelling), 180));
    if (aToks[i].spelling != bToks[i].spelling) {
      return createStringError(
          inconvertibleErrorCode(),
          formatv("token mismatch at index {0}: A='{1}' B='{2}'", i, aDbg, bDbg)
              .str());
    }
   debug("compare", "token match at index {0}: A='{1}' B='{2}'", i, aDbg, bDbg);
  }

  if (aToks.size() != bToks.size()) {
    return createStringError(
        inconvertibleErrorCode(),
        formatv("token count mismatch: A={0} B={1}", aToks.size(), bToks.size())
            .str());
  }

  return Error::success();
}
} // namespace

// ========================== Public entry points ==========================

Expected<std::string> RefoldEngine::Refold(
    const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool onlyCheck, bool noLines, bool strict) {
  if (onlyCheck) {
    if (Error err = compareTokens(aToks, bToks))
      return std::move(err);
    return std::string();
  }

  // Build the refold model based on the parsed JSON object.
  auto mOrErr = RefoldModel::FromJson(rootJson);
  if (!mOrErr)
    return mOrErr.takeError();

  // Construct an engine and run the instance pipeline.
  RefoldEngine engine(std::move(*mOrErr), aSource, aToks, aTokOff, bSource,
                      bToks, bTokOff, noLines, strict);
  return engine.Refold();
}

std::string RefoldEngine::Refold() {
  // Make sure that when we re-lex the A-stream tokens that it matches the token
  // count as listed in the refold map JSON file.
  if (static_cast<size_t>(model_.GetTokensCountA()) != aToks_.size()) {
    fatal("tok",
          "A-stream token count mismatch: model reported {0} tokens, but lexed "
          "sequence (aToks) has {1} tokens.",
          model_.GetTokensCountA(), aToks_.size());
  }

  StringRef tuPath = model_.GetSourcePath();

  info("plan", "REFOLD START tuPath={0} aLen={1} bLen={2} aToks={3} bToks={4}",
       tuPath, aSource_.size(), bSource_.size(), aToks_.size(), bToks_.size());

  // Read in the translation unit file / C source.
  std::unique_ptr<llvm::MemoryBuffer> tuBuffer;
  {
    const auto fullTuPath = lineDirs_.ToAbsolutePath(tuPath);
    auto bufOrErr = MemoryBuffer::getFile(fullTuPath);
    if (!bufOrErr) {
      // Fatal and stop: unreachable past this point.
      fatal("src/load", "failed to read C source: {0} ({1})", fullTuPath,
            bufOrErr.getError().message());
    }

    // Don't need a copy of the bytes here due to lifetime reasoning.
    tuBuffer = std::move(*bufOrErr);
  }
  StringRef tuBytes = tuBuffer->getBuffer();

  constexpr size_t MAX_COLS = 80;
  SmallString<MAX_COLS> sepBuf;
  sepBuf.assign(MAX_COLS, '-');
  StringRef sep = sepBuf;

  // 1) Generate token sequences and A->B anchor map.
  auto aSeq = MapLexemes(aToks_, aTokOff_);
  trace("lcs/aSeq", "aSeq:");
  trace("lcs/aSeq", "=====");
  logFormattedArray<StringRef>(aSeq, /* k */ MAX_COLS,
                                 /* sameWidth */ false,
                                 [](StringRef msg) { trace("lcs/aSeq", msg); });
  trace("lcs/aSeq", sep);

  auto bSeq = MapLexemes(bToks_, bTokOff_);
  trace("lcs/bSeq", "bSeq:");
  trace("lcs/bSeq", "=====");
  logFormattedArray<StringRef>(bSeq, /* k */ MAX_COLS,
                                 /* sameWidth */ false,
                                 [](StringRef msg) { trace("lcs/bSeq", msg); });
  trace("lcs/bSeq", sep);

  // 1b) Compute per-gap ownership depth for A's PP tokens.
  ownerDepthGap_ = ComputeOwnerDepthGapsForPP();
  trace("lcs/ownerGap", "ownerDepthGap:");
  trace("lcs/ownerGap", "==============");
  logFormattedArray<unsigned>(
      ownerDepthGap_, /* k */ MAX_COLS, /* sameWidth */ true,
      [](StringRef msg) { trace("lcs/ownerGap", msg); });
  trace("lcs/ownerGap", sep);

  // 2) LCS over tokens (A → B) with owner-aware cost model.
  auto a2b = diffutils::lcsMapAB(aSeq, bSeq, ownerDepthGap_);
  trace("lcs/a2b", "a2b:");
  trace("lcs/a2b", "====");
  logFormattedArray<int64_t>(a2b, /* k */ MAX_COLS, /* sameWidth */ true,
                         [](StringRef msg) { trace("lcs/a2b", msg); });
  trace("lcs/a2b", sep);

#if 0
  auto writeMap = [](StringRef filename, ArrayRef<int64_t> a2b) {
    std::error_code ec;
    raw_fd_ostream os(filename.str(), ec, llvm::sys::fs::OF_Text);
    if (ec) {
      fatal("a2b/write", "open file '{0}' failed: {1}", filename, ec.message());
    }

    for (int64_t v : a2b)
      os << v << '\n';

    os.flush();
    if (os.has_error()) {
      os.clear_error();
      fatal("a2b/write", "write on file '{0}' failed", filename);
    }
  };

  SmallString<256> lcsMapFile;
  sys::fs::expand_tilde("~/lcsmap.cpp.txt", lcsMapFile);
  writeMap(lcsMapFile, a2b);
#endif

  // Sanity check: map must have a strict ordering.
  int64_t last = -1;
  for (size_t i = 0; i < a2b.size(); ++i) {
    int64_t j = a2b[i];
    if (j < 0)
      continue;
    if (j < last) {
      fatal("lcs/map", "non-monotone map at A[{0}]={1} after {2}", i, j, last);
    }
    last = j;
  }

  debug("lcs", "A={0} toks, B={1} toks", aSeq.size(), bSeq.size());
  size_t mapped = 0;
  for (int64_t v : a2b) {
    if (v >= 0)
      mapped++;
  }
  debug("lcs", "mapped A->B = {0} ({1:F1}%)", mapped,
        100.0 * mapped / std::max<size_t>(1U, aSeq.size()));

  info("plan", "TU={0} includes={1} macroInvocations={2} tokmap={3}", tuPath,
       model_.GetIncludes().size(), model_.GetMacroInvocations().size(),
       model_.GetTokmapByPP().size());

  // 3) Diff hunks (changed A-token intervals -> B-token intervals).
  auto hunks = diffutils::hunksFromMap(a2b, aSeq.size(), bSeq.size());

  // Build *raw-text* byte hunks once; this enables deterministic mapping of PP
  // byte spans from A->B, without inheriting any ambiguity from token-level
  // alignment.
  //
  // This is critical for "insert-only" edits, where token-only LCS diffing can
  // place the insertion at an arbitrary stable point, corrupting subsequent
  // A->B byte span mapping.
  abByteHunks_ = BuildByteHunksFromRawText();

  // DIAGNOSTICS: Output each hunk, when in debug mode, and also perform some
  // input sanitization.
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // Case A: The hunk is logically empty (e.g., a pure deletion)
    if (h.bStart >= h.bEnd) {
      debug("hunks", "#{0} {1:verbose} B=<empty/deleted>", i, h);
      continue;
    }

    // Case B: Hunk indices are out of bounds for the token-to-byte map
    if (h.bEnd >= bTokOff_.size()) {
      fatal("hunks",
            "#{0} {1:verbose} B=OUT-OF-BOUNDS: h.bEnd={2} map.size={3}", i, h,
            h.bEnd, bTokOff_.size());
      continue;
    }

    const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];
    const size_t b1 = bTokOff_[static_cast<size_t>(h.bEnd)];

    // Case C: The token-to-byte map contains sentinels (virtual/synthetic
    // tokens)
    if (b0 == StringRef::npos || b1 == StringRef::npos) {
      fatal("hunks", "#{0} {1:verbose} B=SENTINEL: b0={2} b1={3}", i, h,
            (b0 == StringRef::npos ? "npos" : "valid"),
            (b1 == StringRef::npos ? "npos" : "valid"));
      continue;
    }

    // Case D: Byte offsets are inverted (corrupt map or out-of-order tokens)
    if (b1 < b0) {
      fatal("hunks", "#{0} {1:verbose} B=INVERTED-OFFSETS: b0={2} b1={3}", i, h,
            b0, b1);
      continue;
    }

    // Final physical safety clamp (prevents crashes if map is stale relative to
    // source)
    const size_t lo = std::min(b0, bSource_.size());
    const size_t hi = std::min(b1, bSource_.size());

    StringRef bfrag = bSource_.substr(lo, hi - lo);

    // Happy Path: Log the successfully extracted fragment
    std::string shown = stringutils::showWSWithClip(bfrag, 160);
    debug("hunks", "#{0} {1:verbose} B='{2}'", i, h, shown);
  }

  // 4) Classify hunks and collect per-target edits.
  std::vector<TextEdit> tuEdits;
  DenseMap<uint64_t, IncludeEdits> perInclude; // includeId -> edits
  DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
      macroPatchesByOwner;

  // Merge macro patches by macro-invocation id so multiple arg hunks compose
  // correctly.
  DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
      macroPatchByOwnerByMacroId;

  // Iterate over all hunks:
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // Shape info (pure insert/delete/replace) – LOG ONLY
    bool isIns = (h.aStart == h.aEnd) && (h.bStart < h.bEnd);
    bool isDel = (h.aStart < h.aEnd) && (h.bStart == h.bEnd);
    bool isRep = (h.aStart < h.aEnd) && (h.bStart < h.bEnd);
    debug("classify", "#{0} shape: isIns={1} isDel={2} isRep={3} {4}", i, isIns,
          isDel, isRep, h);

    // a) Segment-aware owner classification: this decides TU vs include vs “no
    // segment”.
    debug("classify", "#{0} -> calling classifyOwnerWithSegments {1}", i, h);
    Owner owner = ClassifyOwnerWithSegments(tuPath, h);
    debug("classify",
          "#{0} ownerFromSegments kind={1} includeId={2} condArmId={3} {4}", i,
          owner.kind, owner.includeId, owner.condArmId, h);

    // b) Macro call-site still has priority over TU/include
    if (auto *m = SmallestCoveringPatchableMacro(h.aStart, h.aEnd)) {
      if (m->invB && m->invE) {
        debug("classify",
              "#{0} -> MACRO invText={1} owner={2} invFile={3} {4})", i,
              m->invText, m->ownerIncludeId, m->invFile, h);
        auto &byMacroId = macroPatchByOwnerByMacroId[m->ownerIncludeId];
        auto existingIt = byMacroId.find(m->id);

        // If found, use the existing replacement; otherwise, use the original
        // text.
        std::string currentInvText =
            (existingIt != byMacroId.end())
                ? existingIt->second.replacement
                : (m->invText ? m->invText->str() : "");
        auto updated = BuildMacroInvocationPatchWholeCover(
            *m, h, currentInvText, macroPatchByOwnerByMacroId);
        if (updated) {
          byMacroId[m->id] = std::move(*updated);
        }
        continue;
      }
    }

    // c) Special case: pure insertions exactly at the boundary between sibling
    // includes that share a common parent. In this case, per policy, the
    // insertion should be attached to the *parent* include so that the
    // refolded C places it between `#include` lines, not inside any child.
    if (isIns) {
      const RefoldModel::IncludeItem *parentBoundaryInc =
          BoundaryParentIncludeForPureInsertion(h);
      if (parentBoundaryInc) {
        auto [it, _] =
            perInclude.try_emplace(parentBoundaryInc->id, parentBoundaryInc);
        IncludePatch patch = BuildIncludeInsertionPatch(*parentBoundaryInc, h);
        it->second.Add(std::move(patch));
        debug("classify",
              "#{0} -> INCLUDE(parent-boundary) inc={1} ({2}) patch={3}", i,
              parentBoundaryInc->id, parentBoundaryInc->resolvedPath, patch);
        continue;
      }

      if (owner.kind == OwnerKind::Include && owner.includeId) {
        std::optional<uint64_t> firstCond =
            model_.FirstConditionalArmStartA(*owner.includeId);
        if (firstCond && h.aStart <= *firstCond) {
          const RefoldModel::IncludeItem *inc =
              model_.GetIncludeById(*owner.includeId);
          // NOTE: `inc` cannot be null if owner has an `includeId`
          auto [it, _] = perInclude.try_emplace(inc->id, inc);
          IncludePatch patch = BuildIncludeInsertionPatch(*inc, h);
          it->second.Add(std::move(patch));
          debug("classify",
                "#{0} → INCLUDE(before first cond) include={1} patch={2}", i,
                inc->id, patch);
          continue;
        }
      }
    }

    debug("classify",
          "#{0} no macro/boundary owner, proceeding with owner.kind={1} "
          "includeId={2}",
          i, owner.kind, owner.includeId);

    // d) Include-owned edit (segment policy already enforced in
    // classifyOwnerWithSegments).
    if (owner.kind == OwnerKind::Include && owner.includeId) {
      const RefoldModel::IncludeItem *inc =
          model_.GetIncludeById(*owner.includeId);
      // NOTE: `inc` cannot be null if owner has an `includeId`
      const std::string incPath = resolveHeaderPath(*inc);

      debug("classify", "#{0} -> INCLUDE id={1} path={2}  {3} (via segments)",
            i, inc->id, incPath, h);

      auto [it, _] = perInclude.try_emplace(inc->id, inc);
      IncludePatch patch = BuildIncludeInsertionPatch(*inc, h);
      debug("include/patch", "#{0} INC {1} patch={2}", i, h, patch);
      it->second.Add(std::move(patch));
      continue;
    }

    // e) TU edit? (segments + existing TU mapping cooperate here).
    // We rely on the token→file map as the source of truth for TU ownership.
    // Segment classification is only used to detect include-owned edits; it
    // should not force a hunk into the TU if any mapped token belongs to a
    // header. So only treat it as TU when the hunk map says so.
    bool mapsToTU = HunkMapsToTU(h.aStart, h.aEnd, tuPath);
    debug("classify",
          "#{0} hunkMapsToTU={1} {2} owner.kind={3} owner.includeId={4}", i,
          mapsToTU, h, owner.kind, owner.includeId);

    if (owner.kind == OwnerKind::TU && !mapsToTU) {
      // Deterministic rule: TU ownership must be supported by provenance. If
      // tokmap-based evidence does not indicate TU ownership, do not force TU
      // edits (even for insertions). Leave owner unresolved so strict mode can
      // surface the deficiency.
      owner = Owner::Unknown();
    }

    if (mapsToTU) {
      if (owner.kind == OwnerKind::Include && owner.includeId) {
        debug(
            "classify",
            "#{0} DIAGNOSTIC: tokmap says TU but segments say INCLUDE(id={1}); "
            "will still treat as TU (tokmap wins).",
            i, owner.includeId);
      }
      auto span = TUByteSpan(h.aStart, h.aEnd, tuPath); // [b,e)
      debug("classify", "#{0} TU-byteSpan=[{1},{2}) for A[{3},{4})", i,
            span->first, span->second, h.aStart, h.aEnd);

      std::string repl;
      if (span) {
        if (h.bStart < h.bEnd) {
          size_t b0 = bTokOff_[h.bStart], b1 = bTokOff_[h.bEnd];
          repl.assign(bSource_.data() + b0, bSource_.data() + b1);

          // Token envelopes start at the first token, so they exclude any
          // intra-line whitespace that precedes that token in B. For a pure
          // insertion at a zero-width TU site, preserve this leading trivia so
          // edits like appending " + 7" after a macro call don't lose the
          // space.
          if (span->first == span->second && h.bStart > 0) {
            size_t p = b0;
            while (p > 0) {
              char c = bSource_[p - 1];
              if (c == ' ' || c == '\t') {
                --p;
                continue;
              }
              break;
            }
            if (p < b0) {
              bool hasSpaceLeft =
                  (span->first > 0 &&
                   (tuBytes[span->first - 1] == ' ' ||
                    tuBytes[span->first - 1] == '\t'));
              if (!hasSpaceLeft)
                repl.insert(0, std::string(bSource_.data() + p, b0 - p));
            }
          }
        }

        // Is this span replacing a TU "gap" (bytes that are all whitespace)?
        std::string original;
        if (span->second > span->first) {
          original.assign(tuBytes.data() + span->first,
                          tuBytes.data() + span->second);
        } else if (span->second < span->first) {
          fatal("tu/span", "invalid TU byte span: [{0},{1})", span->first,
                span->second);
        }

        bool replacingGap =
            !original.empty() && stringutils::isWhitespace(original);

        // If we’re replacing a non-empty TU gap and the inserted text doesn’t
        // start with WS, prefix EXACTLY ONE space from the gap to preserve
        // “return injected” (no double spaces).
        if (replacingGap && !repl.empty() && !stringutils::isWs(repl.front()))
          repl.insert(repl.begin(), ' ');

        // Final boundary padding:
        // - allowLeft only if we did NOT already preserve a gap (avoids
        //   double-space)
        // - always allowRight (covers cases like “…0” + “: 1” at zero-width
        //   sites)
        std::string padded =
            PadAtBoundaries(tuBytes, static_cast<size_t>(span->first),
                            static_cast<size_t>(span->second), std::move(repl),
                            /*allowLeft*/ !replacingGap,
                            /*allowRight*/ true);

        debug("classify",
              "#{0} -> TU  bytes=[{1},{2}) rawRepl='{3}' paddedRepl='{4}'", i,
              span->first, span->second, stringutils::showWSWithClip(repl, 160),
              stringutils::showWSWithClip(padded, 160));

        ResyncOutcome ro = ApplyResyncOrPend(tuBytes, span->first, span->second,
                                             padded, tuPath);
        tuEdits.push_back(TextEdit{span->first, span->second,
                                   std::move(ro.text), std::move(ro.pending)});
        continue;
      } else {
        debug("classify",
              "#{0} TU mapping had nullopt for span; TU edit skipped (behavior "
              "unchanged).",
              i);
      }
    }

    // f) Ownership resolution failed.
    //
    // Deterministic behavior:
    //   * strict mode: fatal (schema deficiency or segment-construction bug)
    //   * non-strict: do not realize includes; attempt TU-only edit if a TU
    //     byte span exists
    if (strict_) {
      fatal("hunks",
            "owner unresolved for changed A-interval [{0},{1}) (segments did "
            "not classify; include guessing disabled)",
            h.aStart, h.aEnd);
    }

    debug("classify",
          "#{0} owner unresolved (not TU/macro/segment). Conservative TU-only "
          "attempt (no include realization).",
          i);

    if (auto span = TUByteSpan(h.aStart, h.aEnd, tuPath)) { // [b, e)
      std::string repl;
      if (isDel) {
        repl = "";
      } else {
        const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];
        const size_t b1 = bTokOff_[static_cast<size_t>(h.bEnd)];
        repl.assign(bSource_.data() + b0, bSource_.data() + b1);
      }

      // Token envelopes start at the first token, so they exclude any
      // intra-line whitespace that precedes that token in B. For a pure
      // insertion (empty TU span), preserve that leading trivia so statement-
      // local formatting (e.g. "FOO(...) + 7") isn't collapsed to
      // "FOO(...)+ 7".
      if (!isDel && span->first == span->second && h.bStart < h.bEnd &&
          h.bStart > 0) {
        const size_t bTokStart = static_cast<size_t>(h.bStart);
        const size_t b0 = bTokOff_[bTokStart];
        size_t p = b0;
        while (p > 0) {
          char c = bSource_[p - 1];
          if (c == ' ' || c == '\t') {
            --p;
            continue;
          }
          break;
        }
        if (p < b0) {
          const bool tuHasSpaceLeft =
              span->first > 0 &&
              (tuBytes[span->first - 1] == ' ' || tuBytes[span->first - 1] == '\t');
          if (!tuHasSpaceLeft)
            repl.insert(0, std::string(bSource_.data() + p, b0 - p));
        }
      }

      // If we are replacing whitespace-only text in the TU, we prefer to
      // preserve the existing TU gap whitespace rather than introducing new
      // whitespace from B.
      bool replacingGap = false;
      if (span->first < span->second) {
        std::string original(tuBytes.data() + span->first,
                             tuBytes.data() + span->second);
        replacingGap = !original.empty() && stringutils::isWhitespace(original);
        if (replacingGap) {
          // Preserve exactly the gap as the replacement.
          repl = std::move(original);
        }
      } else if (span->second < span->first) {
        fatal("tu/span", "invalid TU byte span: [{0},{1})", span->first,
              span->second);
      }

      // If we're replacing a non-empty TU gap and the inserted text doesn't
      // start with WS, prefix EXACTLY ONE space from the gap to preserve
      // “return injected” (no double spaces).
      if (replacingGap && !repl.empty() && !stringutils::isWs(repl.front()))
        repl.insert(repl.begin(), ' ');

      // Keep a copy for logging; PadAtBoundaries consumes via move.
      std::string rawRepl = repl;

      std::string padded =
          PadAtBoundaries(tuBytes, static_cast<size_t>(span->first),
                          static_cast<size_t>(span->second), std::move(repl),
                          /*allowLeft*/ !replacingGap,
                          /*allowRight*/ true);

      debug("classify",
            "#{0} -> TU (conservative) bytes=[{1},{2}) rawRepl='{3}' "
            "paddedRepl='{4}'",
            i, span->first, span->second,
            stringutils::showWSWithClip(rawRepl, 160),
            stringutils::showWSWithClip(padded, 160));

      ResyncOutcome ro =
          ApplyResyncOrPend(tuBytes, span->first, span->second, padded, tuPath);
      tuEdits.push_back(TextEdit{span->first, span->second, std::move(ro.text),
                                 std::move(ro.pending)});
      continue;
    }

    debug("classify",
          "#{0} dropping edit {1}: owner unresolved and no TU byte span "
          "available (no include guessing).",
          i, h);
    continue;
  }

  // Materialize merged macro patches into the list buckets expected by later
  // phases.
  for (auto &outerEntry : macroPatchByOwnerByMacroId) {
    auto &finalPatches = macroPatchesByOwner[outerEntry.first];
    auto &patchesById = outerEntry.second;
    append_range(finalPatches, make_second_range(patchesById));
  }

  debug("plan", "perInclude.size={0} macroOwners={1} tuEdits(initial)={2}",
        perInclude.size(), macroPatchesByOwner.size(), tuEdits.size());

  // Normalize/coalesce include-side insertions.
  OrderIncludeInsertions(perInclude);

  // 5) Materialize include expansions bottom-up (nested first). Build child
  // lists by parent include id.
  DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>> children;
  for (const auto &ii : model_.GetIncludes()) {
    if (ii.parent)
      children[*ii.parent].push_back(&ii);
  }

  trace("include/tree", "BEGIN include children");
  for (const auto &[parentId, items] : children) {
    std::string pName = "#" + std::to_string(parentId);
    for (const RefoldModel::IncludeItem *child : items) {
      debug("include/tree",
            "{0} -> #{1} target={2} resolved={3} sitePath={4} site=[{5},{6}) "
            "cover=[{7},{8})",
            pName, child->id, child->target, child->resolvedPath,
            child->sitePath, child->siteB, child->siteE, child->cover.begin,
            child->cover.end);
    }
  }
  trace("include/tree", "END include children");

  // Cache for realized expansion text per include id.
  DenseMap<uint64_t, std::string> includeExpansion;

  // Build the set of include-ids that must be realized.
  DenseSet<uint64_t> seeds;

  // (a) Direct include edits.
  auto perIncludeKeys = make_first_range(perInclude);
  seeds.insert(perIncludeKeys.begin(), perIncludeKeys.end());

  // (b) Macro-owned work INSIDE headers (ownerIncludeId != null).
  for (auto &kv : macroPatchesByOwner) {
    if (kv.first)
      seeds.insert(*kv.first);
  }

  // (c) Pull in all ancestors up to the TU.
  // TODO: is there a more efficient way to do this?
  for (uint64_t id : std::vector<uint64_t>(seeds.begin(), seeds.end())) {
    const auto *cur = model_.GetIncludeById(id);
    while (cur && cur->parent) {
      seeds.insert(*cur->parent);
      cur = model_.GetIncludeById(*cur->parent);
    }
  }

  std::vector<uint64_t> seedsVec(seeds.begin(), seeds.end());
  trace("include/mat", "seeds:");
  trace("include/mat", "======");
  logFormattedArray<uint64_t>(seedsVec, /* k */ MAX_COLS,
                              /* sameWidth */ false,
                              [](StringRef msg) { trace("include/mat", msg); });
  trace("include/mat", sep);

  // (d) Realize each include once (memoization lives inside
  // MaterializeIncludeExpansion).
  for (uint64_t incId : seeds) {
    debug("include/mat", "materialize seed include #{0}", incId);
    MaterializeIncludeExpansion(incId, perInclude, macroPatchesByOwner,
                                children, includeExpansion);
  }

  // 6a) TU macro patches (ownerIncludeId == std::nullopt) and include
  // expansions at TU sites.
  if (auto it = macroPatchesByOwner.find(std::nullopt);
      it != macroPatchesByOwner.end() && !it->second.empty()) {
    // Create a local copy to sort
    auto tuMacroPatches = it->second;

    // 1. Sort patches by start offset, then by length (descending) to ensure we
    // process the "outermost" (largest) macros first.
    std::sort(tuMacroPatches.begin(), tuMacroPatches.end(),
              [](const MacroPatch &p1, const MacroPatch &p2) {
                if (p1.invStart != p2.invStart)
                  return p1.invStart < p2.invStart;
                return p1.invEnd > p2.invEnd;
              });

    SmallVector<MacroPatch, 16> accepted;
    for (const auto &mp : tuMacroPatches) {
      bool isShadowed = false;
      for (const auto &acc : accepted) {
        // If this patch is contained within one we already accepted, skip it.
        if (mp.invStart >= acc.invStart && mp.invEnd <= acc.invEnd) {
          isShadowed = true;
          break;
        }

        // Partial overlaps should never occur (macro invocation sites are
        // either disjoint or nested). If they do, fail fast rather than
        // producing order-dependent behavior.
        if (mp.invStart < acc.invEnd && acc.invStart < mp.invEnd) {
          fatal("macro/tu",
                "overlapping TU macro patches: mp=[{0},{1}) acc=[{2},{3})",
                mp.invStart, mp.invEnd, acc.invStart, acc.invEnd);
        }
      }

      if (!isShadowed) {
        accepted.push_back(mp);
        debug("macro/tu", "  TU macro patch accepted inv=[{0},{1}) replLen={2}",
              mp.invStart, mp.invEnd, mp.replacement.size());
        ResyncOutcome ro = ApplyResyncOrPend(tuBytes, mp.invStart, mp.invEnd,
                                             mp.replacement, tuPath);
        tuEdits.push_back(TextEdit{mp.invStart, mp.invEnd, std::move(ro.text),
                                   std::move(ro.pending)});
      } else {
        trace("macro/tu", "  TU macro patch shadowed (skipped) inv=[{0},{1})",
              mp.invStart, mp.invEnd);
      }
    }
  }

  // 6b) TU include expansions: includes with parent == null and site in TU,
  // only if we realized an expansion.
  for (const auto &kv : includeExpansion) {
    const auto *inc = model_.GetIncludeById(kv.first);
    if (!inc)
      continue;
    if (!inc->parent && PathsEqual(inc->sitePath, tuPath)) {
      debug("include/tu", "TU include expansion inc#{0} site=[{1},{2}) len={3}",
            inc->id, inc->siteB, inc->siteE, kv.second.size());
      std::string headerPath = resolveHeaderPath(*inc);
      std::string wrapped = lineDirs_.WrapIncludeExpansion(
          headerPath, tuPath, stringutils::lineAtOffset(tuBytes, inc->siteE),
          kv.second);
      tuEdits.push_back(
          TextEdit{inc->siteB, inc->siteE, std::move(wrapped), std::nullopt});
    }
  }

  // Apply TU edits in descending order of start offset.
  debug("tu/apply", "applying {0} TU edits", tuEdits.size());
  std::string tuResult = ApplyTextEditsWithPendingResync(tuBytes, tuEdits);
  debug("plan", "REFOLD DONE tuResultLen={0}", tuResult.size());
  return tuResult;
}

// ================== A ↔ B token mapping & diff utilities ===================

std::vector<StringRef> RefoldEngine::MapLexemes(ArrayRef<PPTok> toks,
                                                ArrayRef<size_t> offs) {
  std::vector<StringRef> out;
  out.reserve(toks.size());
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const auto &s = toks[i].spelling;
    if (stringutils::isWhitespace(s)) {
      // We should never encounter a whitespace token
      fatal("map/lexemes", "token at index {0} is whitespace", i);
    } else {
      out.emplace_back(StringRef(s));
    }
  }
  return out;
}

std::vector<uint32_t> RefoldEngine::ComputeOwnerDepthGapsForPP() {
  // aTokOff.size() == (#tokens) + 1 (sentinel). LCS expects N == #tokens,
  // and ownerDepthGap.size() == N + 1.
  const size_t N = aTokOff_.size() - 1;
  std::vector<uint32_t> ownerDepthGap(N + 1, 0);

  for (size_t k = 0; k <= N; ++k) {
    // --------------------------- Include depth ----------------------------
    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;

    if (k > 0) {
      leftInc = model_.InnermostIncludeAtPP(k - 1);
    }
    if (k < N) {
      rightInc = model_.InnermostIncludeAtPP(k);
    }

    std::optional<uint64_t> lca =
        model_.LeastCommonAncestorInclude(leftInc, rightInc);
    uint32_t incDepth = model_.GetIncludeDepth(lca);

    // ------------------------- Conditional depth --------------------------
    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (k > 0) {
      leftArmRef = model_.FindArmRefAtPP(k - 1);
    }
    if (k < N) {
      rightArmRef = model_.FindArmRefAtPP(k);
    }

    uint32_t leftCondDepth =
        leftArmRef ? model_.GetCondArmDepth(leftArmRef->arm->id) : 0;
    uint32_t rightCondDepth =
        rightArmRef ? model_.GetCondArmDepth(rightArmRef->arm->id) : 0;
    uint32_t condDepth = std::min(leftCondDepth, rightCondDepth);

    ownerDepthGap[k] = incDepth + condDepth;
  }

  return ownerDepthGap;
}

// ============================= Boundary helpers ==============================

bool RefoldEngine::BoundaryGlues(char left, char right) {
  const bool leftId = stringutils::isIdentPart(left);
  const bool rightId = stringutils::isIdentPart(right);
  if (leftId && rightId)
    return true; // e.g., return + injected → returninjected
  if (leftId && right == '(')
    return true; // e.g., foo(…)
  // Operator-like punctuation right after an identifier/number benefits from a
  // space
  static constexpr char OPS[] = ":?+-*/%&|^<>=!";
  if (leftId && std::char_traits<char>::find(OPS, sizeof(OPS) - 1, right))
    return true; // e.g., 0: → 0 :
  return false;
}

std::string RefoldEngine::PadAtBoundaries(StringRef base, size_t start,
                                          size_t end, std::string text,
                                          bool allowLeft, bool allowRight) {
  const auto f = stringutils::firstNonWsIdx(text);
  const auto l = stringutils::lastNonWsIdx(text);

  // If text is empty or only whitespace, there's no content to "glue"
  if (!f || !l)
    return text;

  // Character in base immediately to the left/right of the replacement range
  const char leftC =
      (start > 0 && start <= base.size()) ? base[start - 1] : '\0';
  const char rightC = end < base.size() ? base[end] : '\0';

  // Check for existing whitespace at the edges of the provided text
  const bool hasLeadingWS = (*f > 0);
  const bool hasTrailingWS = (*l + 1 < text.size());

  // Determine if padding is needed BEFORE modifying the string to avoid index
  // drift
  bool addLeftSpace =
      allowLeft && !hasLeadingWS && BoundaryGlues(leftC, text[*f]);
  bool addRightSpace =
      allowRight && !hasTrailingWS && BoundaryGlues(text[*l], rightC);

  if (addLeftSpace)
    text.insert(0, 1, ' ');

  if (addRightSpace)
    text.push_back(' ');

  return text;
}

// ===================== Owner resolution & TU mapping ======================

RefoldEngine::Owner
RefoldEngine::ClassifyOwnerWithSegments(StringRef tuPath,
                                        const diffutils::Hunk &h) {
  uint64_t a0 = h.aStart;
  uint64_t a1 = h.aEnd;

  debug("segments",
        "ENTER classifyOwnerWithSegments tuPath={0} A[{1},{2}) (isEmpty={3})",
        tuPath, a0, a1, a0 == a1);

  // Insertion ownership:
  //
  // If the PP gap aligns with a stable TU slot boundary (include boundary or
  // conditional-arm boundary), defer to the segment-based classification (using
  // TU byte anchoring).
  //
  // Otherwise, if both sides of the gap are unambiguously within the same
  // include's PP coverage, treat the insertion as include-owned.
  if (a0 == a1) {
    if (auto slotAnchor = AnchorToExactSlotBoundaryFromPPGap(tuPath, a0)) {
      std::optional<uint64_t> leftInc =
          (a0 > 0) ? model_.InnermostIncludeAtPP(a0 - 1) : std::nullopt;
      const uint64_t maxPP = model_.GetTokensCountA();
      std::optional<uint64_t> rightInc =
          a0 < maxPP ? model_.InnermostIncludeAtPP(a0) : std::nullopt;

      if (leftInc && rightInc && *leftInc == *rightInc) {
        trace("segments",
              "    insertion gap PP={0} classified as INCLUDE id={1} (left={2} "
              "right={3})",
              a0, rightInc, leftInc, rightInc);
        return Owner::Include(*rightInc);
      }
    }
  }

  // First, get the TU byte span for this hunk. Even when the hunk ultimately
  // belongs to a header, we still anchor via the TU span because segments for
  // includes and conditional arms in that header are projected into the TU
  // through slots.
  auto span = TUByteSpan(a0, a1, tuPath); // [b, e)

  // If there is no truthful TU anchor (no TU tokens in the range, and the
  // insertion cannot be safely anchored in TU), classify purely in PP space.
  if (!span) {
    const bool isInsert = (a0 == a1);
    const size_t n = model_.GetTokensCountA();

    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;

    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (isInsert) {
      if (a0 > 0) {
        leftInc = model_.InnermostIncludeAtPP(a0 - 1);
        leftArmRef = model_.FindArmRefAtPP(a0 - 1);
      }
      if (static_cast<size_t>(a0) < n) {
        rightInc = model_.InnermostIncludeAtPP(a0);
        rightArmRef = model_.FindArmRefAtPP(a0);
      }
    } else {
      leftInc = model_.InnermostIncludeAtPP(a0);
      rightInc = model_.InnermostIncludeAtPP(a1 - 1);
      leftArmRef = model_.FindArmRefAtPP(a0);
      rightArmRef = model_.FindArmRefAtPP(a1 - 1);
    }

    std::optional<uint64_t> lcaInc =
        model_.LeastCommonAncestorInclude(leftInc, rightInc);

    std::optional<uint64_t> condArmId;
    if (leftArmRef && rightArmRef && leftArmRef->arm && rightArmRef->arm &&
        leftArmRef->arm->id == rightArmRef->arm->id) {
      condArmId = leftArmRef->arm->id;
    }

    if (lcaInc) {
      trace("segments",
            "  no TU anchor for hunk [{0},{1}); PP-only owner INCLUDE id={2} "
            "(condArmId={3})",
            a0, a1, lcaInc, condArmId);
      return Owner::Include(*lcaInc, condArmId);
    }

    // If we cannot establish an include owner, fall back to TU (this should
    // only happen at TU boundaries where slot anchoring is missing).
    trace("segments",
          "  no TU anchor for hunk [{0},{1}); PP-only owner TU (condArmId={2})",
          a0, a1, condArmId);
    return Owner::TU(condArmId);
  }

  uint64_t b = span->first, e = span->second;
  if (b > e)
    std::swap(b, e);

  trace("segments", "  TU byte span for A[{0},{1}) in {2} = [{3},{4})", a0, a1,
        tuPath, b, e);

  // Build (or fetch) all segments projected into tuPath.
  ArrayRef<RefoldModel::Segment> segs = model_.GetSegmentsForFile(tuPath);
  if (segs.empty()) {
    debug("segments", "  no segments for file={0}; owner UNKNOWN", tuPath);
    return Owner::Unknown();
  }

  // Step 1: find all candidate segments.
  //
  // For non-empty hunks: any overlap with [b,e) is a candidate.
  // For pure INSERTs: the span is typically zero-width; treat the anchor as a
  // point and include segments that contain the probe, with a right-closed
  // convention to enable parent selection at boundaries.
  const bool isInsert = (a0 == a1);
  const uint64_t probe = b;

  std::vector<const RefoldModel::Segment *> hits;
  for (const auto &s : segs) {
    if (!isInsert) {
      if (s.e <= b || e <= s.b)
        continue; // no intersection
      hits.push_back(&s);
    } else {
      if (s.b <= probe && probe <= s.e)
        hits.push_back(&s);
    }
  }

  if (hits.empty()) {
    debug("segments",
          "  no candidate segments for {0} span [{1},{2}) (probe={3}, "
          "isInsert={4}); owner UNKNOWN",
          tuPath, b, e, probe, isInsert);
    return Owner::Unknown();
  }

  // Step 2: pick the smallest (most specific) segment among the hits.
  const RefoldModel::Segment *selected = hits[0];
  for (size_t i = 1; i < hits.size(); ++i) {
    const auto *s = hits[i];
    uint64_t sLen = s->e - s->b;
    uint64_t selLen = selected->e - selected->b;

    if (sLen < selLen) {
      selected = s;
    } else if (sLen == selLen) {
      if (s->b < selected->b) {
        selected = s;
      } else if (s->b == selected->b && s->e < selected->e) {
        selected = s;
      }
    }
  }

  if (!selected->ownerIncludeId) {
    trace("segments",
          "  selected segment [{0},{1}) (probe={2}) owner TU (condArmId={3}) "
          "for hunk [{4},{5})",
          selected->b, selected->e, probe, selected->ownerCondArmId, a0, a1);
    return Owner::TU(selected->ownerCondArmId);
  } else {
    trace("segments",
          "  selected segment [{0},{1}) (probe={2}) owner INCLUDE id={3} "
          "(condArmId={4}) for hunk [{5},{6})",
          selected->b, selected->e, probe, selected->ownerIncludeId,
          selected->ownerCondArmId, a0, a1);
    return Owner::Include(*selected->ownerIncludeId, selected->ownerCondArmId);
  }
}

bool RefoldEngine::IsInvocationInsideDefineDirective(
    const RefoldModel::MacroInvocation &m) const {
  if (!m.invFile || !m.invB || !m.invE)
    return false;

  // RefoldModel exposes directives; MacroDirective.subkind is expected to be
  // "define" based on your schema usage elsewhere.
  for (const auto &d : model_.GetMacroDirectives()) {
    if ("#define" != d.subkind)
      continue;
    if (d.sitePath.empty())
      continue;
    if (!m.invFile || !PathsEqual(*m.invFile, d.sitePath))
      continue;

    // If the invocation byte range lies within the #define's site range, treat
    // it as non-patchable.
    if (*m.invB >= d.siteB && *m.invE <= d.siteE)
      return true;
  }

  return false;
}

const RefoldModel::MacroInvocation *
RefoldEngine::SmallestCoveringPatchableMacro(uint64_t aStart,
                                             uint64_t aEnd) const {
  const RefoldModel::MacroInvocation *best = nullptr;
  uint64_t bestLen = std::numeric_limits<uint64_t>::max();

  for (const auto &m : model_.GetMacroInvocations()) {
    // Check if this macro covers the token range [aStart, aEnd)
    if (!m.Covers(aStart, aEnd))
      continue;

    // Must be patchable at a real call site.
    if (!m.invB || !m.invE || !m.invText)
      continue;

    // CRITICAL: never patch invocations that are spelled inside a #define
    // directive.
    if (IsInvocationInsideDefineDirective(m))
      continue;

    // Deterministic selection: smallest cover wins, ties broken by ID.
    uint64_t len = m.cover.end - m.cover.begin;
    if (!best || len < bestLen || (len == bestLen && m.id < best->id)) {
      best = &m;
      bestLen = len;
    }
  }

  return best;
}

bool RefoldEngine::HunkMapsToTU(uint64_t a0, uint64_t a1,
                                StringRef tuPath) const {
  trace("tu/own", "hunkMapsToTU: check ownership for A[{0},{1}) tu={2}", a0, a1,
        tuPath);
  bool sawAnyTU = false;
  const auto &tokmapByPP = model_.GetTokmapByPP();
  for (uint64_t pp = a0; pp < a1; ++pp) {
    auto it = tokmapByPP.find(pp);
    if (it == tokmapByPP.end())
      continue; // ignore unmapped (spaces/tabs/newlines)
    const auto &t = it->second;
    if (!PathsEqual(t.file, tuPath)) {
      trace("tu/own",
            "hunkMapsToTU: A[{0},{1}) hits non-TU mapping at pp={2} file={3} "
            "(tu={4}) -> false",
            a0, a1, pp, t.file, tuPath);
      return false; // spans a non-TU mapping
    }
    sawAnyTU = true;
  }

  // Non-empty range: if we only saw TU mappings (or nothing but whitespace),
  // then the hunk maps to the TU. Otherwise it mapped to some header above.
  if (a0 != a1) {
    if (!sawAnyTU) {
      trace("tu/own",
            "hunkMapsToTU: A[{0},{1}) has no TU-mapped tokens "
            "(unmapped/whitespace-only) -> false",
            a0, a1);
    }
    return sawAnyTU;
  }

  // INSERTION (A gap): determine TU ownership without "nearest-neighbor
  // snapping". In strict mode, consult only immediate neighbors (pp-1, pp). If
  // the PP gap is covered by an include expansion, treat it as header-owned.
  uint64_t pp = a0;

  if (auto slotAnchor = AnchorToExactSlotBoundaryFromPPGap(tuPath, pp)) {
    trace("hunk",
          "    insertion gap PP={0} mapsToTU via slot boundary TU byte {1}", pp,
          slotAnchor);
    return true;
  }

  if (strict_) {
    // Any PP gap inside an include expansion cannot be a TU insertion.
    if (IncludeIdCoveringPPIndex(pp)) {
      return false;
    }

    if (pp < model_.GetTokensCountA()) {
      auto rightIt = tokmapByPP.find(pp);
      if (rightIt != tokmapByPP.end()) {
        return PathsEqual(tuPath, rightIt->second.file);
      }
    }

    if (pp > 0 && pp - 1 < tokmapByPP.size()) {
      auto leftIt = tokmapByPP.find(pp - 1);
      if (leftIt != tokmapByPP.end()) {
        return PathsEqual(tuPath, leftIt->second.file);
      }
    }

    // No immediate neighbor evidence -> not TU (caller may fatal in strict
    // mode).
    return false;
  }

  // Non-strict: bounded best-effort behavior.
  //
  // Historically we scanned arbitrarily far to find the nearest mapped token
  // and inferred TU ownership from that token. With clang now emitting author-
  // itative slot.pp for boundary-like slots, we can be much more conservative
  // here: never snap across an include expansion, and only probe a small bound-
  // ed window when immediate neighbors are unmapped whitespace.
  if (IncludeIdCoveringPPIndex(pp)) {
    return false;
  }

  // Prefer immediate neighbors first.
  if (pp < model_.GetTokensCountA()) {
    auto rightIt = tokmapByPP.find(pp);
    if (rightIt != tokmapByPP.end()) {
      return PathsEqual(tuPath, rightIt->second.file);
    }
  }

  if (pp > 0 && pp - 1 < tokmapByPP.size()) {
    auto leftIt = tokmapByPP.find(pp - 1);
    if (leftIt != tokmapByPP.end()) {
      return PathsEqual(tuPath, leftIt->second.file);
    }
  }

  // Stop scanning once ownership changes (ownerDepthGap) and cap the scan as a failsafe.
  static constexpr uint64_t MAX_SNAP_DISTANCE = 64;

  const bool haveOwnerGaps =
      (ownerDepthGap_.size() == model_.GetTokensCountA() + 1);
  const uint32_t wantOwner =
      (haveOwnerGaps && pp < ownerDepthGap_.size()) ? ownerDepthGap_[pp] : 0;

  const RefoldModel::TokMapEntry* left = nullptr;
  for (uint64_t d = 2; d <= MAX_SNAP_DISTANCE; ++d) {
    if (pp < d) // Prevent unsigned underflow
      break;
    if (haveOwnerGaps) {
      const uint64_t gap = pp - (d - 1);
      if (gap < ownerDepthGap_.size() && ownerDepthGap_[gap] != wantOwner)
        break;
    }
    auto it = tokmapByPP.find(pp - d);
    if (it != tokmapByPP.end()) {
      left = &it->second;
      break;
    }
  }

  const RefoldModel::TokMapEntry* right = nullptr;
  const uint64_t maxPP = model_.GetTokensCountA();
  for (uint64_t d = 1; d <= MAX_SNAP_DISTANCE; ++d) {
    uint64_t ppR = pp + d;
    if (ppR >= maxPP)
      break;
    if (haveOwnerGaps && ppR < ownerDepthGap_.size() && ownerDepthGap_[ppR] != wantOwner)
      break;
    auto it = tokmapByPP.find(ppR);
    if (it != tokmapByPP.end()) {
      right = &it->second;
      break;
    }
  }

  if (left) {
    if (PathsEqual(tuPath, left->file))
      return true;
    return false;
  }
  if (right) {
    if (PathsEqual(tuPath, right->file))
      return true;
    return false;
  }

  // Completely unmapped gap: default to TU in non-strict mode (best effort).
  return true;
}

std::optional<uint64_t>
RefoldEngine::AnchorToExactSlotBoundaryFromPPGap(StringRef tuPath,
                                                   uint64_t ppGap) const {
  // Candidate record for potential anchor points
  struct Cand {
    uint64_t pp; // PP coordinate for the boundary
    uint64_t b;  // TU byte coordinate (possibly adjusted)
    const RefoldModel::Slot *slot;

    Cand(uint64_t pp, uint64_t b, const RefoldModel::Slot *slot)
        : pp(pp), b(b), slot(slot) {}
  };

  // Read TU text for newline-aware slot adjustment
  auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(tuPath));
  if (!bufOrErr) {
    fatal("slot/anchor", "unable to read TU: {0}", tuPath);
    // Should be unreachable!
  }
  StringRef tuText = bufOrErr.get()->getBuffer();

  // Helper to adjust slots that terminate on directive newlines
  auto adjustSlot = [&tuText](const RefoldModel::Slot *s) -> uint64_t {
    uint64_t b = s->b;

    bool needsAdjustment =
        StringSwitch<bool>(s->kind)
            .Cases("after_include", "after_last_include", "arm_end", true)
            .Default(false);
    if (needsAdjustment)
      return b;

    if (b >= tuText.size())
      return b;

    char c = tuText[b];
    if (c == '\n')
      return b + 1;
    if (c == '\r') {
      if ((size_t)(b + 1) < tuText.size() && tuText[b + 1] == '\n')
        return b + 2;
      return b + 1;
    }
    return b;
  };

  std::vector<Cand> cands;

  // 1) Explicit TU slots that already carry 'pp'
  for (const auto &s :
       model_.FindSlots(tuPath, std::nullopt, std::nullopt, std::nullopt)) {
    if (!s->pp)
      continue;

    bool isBoundary =
        StringSwitch<bool>(s->kind)
            .Cases("file_begin", "file_end", "before_include", "after_include",
                   "after_last_include", "arm_begin", "arm_end", true)
            .Default(false);
    if (isBoundary) {
      cands.emplace_back(*s->pp, adjustSlot(s), s);
    }
  }

  // ---------------------------------------------------------------------------
  // NOTE: Slot.pp is now produced by clang for boundary-like slots (includes,
  // arms, file boundaries). We therefore intentionally do NOT reconstruct PP
  // coordinates from include/conditional metadata on the consumer side, since
  // that can diverge from the producer's authoritative view in edge cases
  // (nested includes, re-entrant conditionals, etc.).
  // ---------------------------------------------------------------------------

  if (cands.empty())
    return std::nullopt;

  // Filter for EXACT matches to the ppGap
  std::vector<const Cand *> exact;
  for (const auto &c : cands) {
    if (c.pp == ppGap)
      exact.push_back(&c);
  }

  if (exact.empty())
    return std::nullopt;

  // Priority tie-breaking logic
  auto getPriority = [](StringRef kind) -> unsigned {
    if (kind == "before_include")
      return 0;
    if (kind == "after_include")
      return 1;
    if (kind == "after_last_include")
      return 2;
    if (kind == "arm_begin")
      return 3;
    if (kind == "arm_end")
      return 4;
    if (kind == "file_begin")
      return 5;
    if (kind == "file_end")
      return 6;
    return 100;
  };

  const Cand *best = nullptr;
  for (const auto *c : exact) {
    if (!best) {
      best = c;
      continue;
    }
    unsigned pc = getPriority(c->slot->kind);
    unsigned pb = getPriority(best->slot->kind);

    // Tie-break: Priority -> Byte Offset -> Slot ID
    if (std::tie(pc, c->b, c->slot->id) <
        std::tie(pb, best->b, best->slot->id)) {
      best = c;
    }
  }

  return best ? std::optional<uint64_t>(best->b) : std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::TUByteSpan(uint64_t a0, uint64_t a1, StringRef tuPath) const {
  if (a0 > a1)
    std::swap(a0, a1);

  const bool isEmpty = (a0 == a1);
  const auto &tokmapByPP = model_.GetTokmapByPP();

  // For pure insertions, first prefer an explicit slot boundary (file_begin,
  // before_include, after_include, arm_begin, arm_end, file_end, ...).
  if (isEmpty) {
    if (auto slotAnchor = AnchorToExactSlotBoundaryFromPPGap(tuPath, a0)) {
      return {{*slotAnchor, *slotAnchor}};
    }
  }

  // Non-empty: compute min/max over TU-mapped subset only.
  uint64_t minB = std::numeric_limits<uint64_t>::max();
  uint64_t maxE = 0;
  bool foundTuToken = false;

  for (uint64_t i = a0; i < a1; ++i) {
    auto it = tokmapByPP.find(i);
    if (it == tokmapByPP.end())
      continue;

    const auto &ent = it->second;
    if (!PathsEqual(ent.file, tuPath))
      continue;

    if (ent.b < minB)
      minB = ent.b;
    if (ent.e > maxE)
      maxE = ent.e;
    foundTuToken = true;
  }

  if (foundTuToken) {
    return {{minB, maxE}};
  }

  // Empty insertion with no slot anchor: only anchor to a TU neighbor token
  // when the insertion is TU-owned. If either neighbor is in a header/include,
  // we must NOT fabricate a TU interval (that causes include insertions to snap
  // to TU boundaries).
  if (isEmpty) {
    // INSERTION (A gap): in strict mode, do not "snap" to distant mapped
    // tokens. Consult only immediate neighbors (pp-1, pp). If the PP gap is
    // covered by an include expansion, treat it as header-owned (no TU span).
    uint64_t pp = a0;

    const auto &tokmapByPP = model_.GetTokmapByPP();
    if (strict_) {
      if (IncludeIdCoveringPPIndex(pp)) {
        return std::nullopt;
      }

      if (pp < model_.GetTokensCountA()) {
        auto rightIt = tokmapByPP.find(pp);
        if (rightIt != tokmapByPP.end()) {
          const auto &right = rightIt->second;
          if (PathsEqual(tuPath, right.file)) {
            return {{right.b, right.b}};
          }
        }
      }

      if (pp > 0 && static_cast<size_t>(pp - 1) < tokmapByPP.size()) {
        auto leftIt = tokmapByPP.find(pp - 1);
        if (leftIt != tokmapByPP.end()) {
          const auto &left = leftIt->second;
          if (PathsEqual(tuPath, left.file)) {
            return {{left.e, left.e}};
          }
        }
      }

      return std::nullopt;
    }

    // Non-strict: bounded best-effort behavior (avoid long-range snapping).
    //
    // With slot.pp covering include/arm/file boundaries, the remaining use case for snapping is a
    // pure insertion inside the TU where immediate neighbors are unmapped whitespace. Bound the
    // probe window so we do not accidentally "jump" across regions and mis-own the insertion.

    // Any PP gap inside an include expansion is header-owned (no TU span).
    if (IncludeIdCoveringPPIndex(pp)) {
      return std::nullopt;
    }

    if (pp < model_.GetTokensCountA()) {
      auto rightIt = tokmapByPP.find(pp);
      if (rightIt != tokmapByPP.end()) {
        const auto &right = rightIt->second;
        if (PathsEqual(tuPath, right.file)) {
          return {{right.b, right.b}};
        }
      }
    }

    if (pp > 0 && static_cast<size_t>(pp - 1) < tokmapByPP.size()) {
      auto leftIt = tokmapByPP.find(pp - 1);
      if (leftIt != tokmapByPP.end()) {
        const auto &left = leftIt->second;
        if (PathsEqual(tuPath, left.file)) {
          return {{left.e, left.e}};
        }
      }
    }

    // Stop scanning once ownership changes (ownerDepthGap) and cap the scan as a failsafe.
    constexpr uint64_t MAX_SNAP_DISTANCE = 64;

    const bool haveOwnerGaps =
        (ownerDepthGap_.size() == model_.GetTokensCountA() + 1);
    const uint32_t wantOwner =
        (haveOwnerGaps && pp < ownerDepthGap_.size()) ? ownerDepthGap_[pp] : 0;

    const RefoldModel::TokMapEntry *left = nullptr;
    uint64_t dLeft = std::numeric_limits<uint64_t>::max();
    for (uint64_t d = 2; d <= MAX_SNAP_DISTANCE; ++d) {
      if (pp < d)
        break;
      if (haveOwnerGaps) {
        const uint64_t gap = pp - (d - 1);
        if (gap < ownerDepthGap_.size() && ownerDepthGap_[gap] != wantOwner)
          break;
      }
      auto it = tokmapByPP.find(pp - d);
      if (it != tokmapByPP.end()) {
        const auto& ent = it->second;
        left = &ent;
        dLeft = d;
        break;
      }
    }

    const RefoldModel::TokMapEntry *right = nullptr;
    uint64_t dRight = std::numeric_limits<uint64_t>::max();
    const uint64_t ppCount = model_.GetTokensCountA();
    for (uint64_t d = 1; d <= MAX_SNAP_DISTANCE; ++d) {
      uint64_t i = pp + d;
      if (i >= ppCount)
        break;
      if (haveOwnerGaps && i < ownerDepthGap_.size() && ownerDepthGap_[i] != wantOwner)
        break;
      auto it = tokmapByPP.find(i);
      if (it != tokmapByPP.end()) {
        const auto& ent = it->second;
        right = &ent;
        dRight = d;
        break;
      }
    }

    // If either neighbor points into a header/include, we must NOT fabricate a TU span.
    if (left && !PathsEqual(tuPath, left->file))
      return std::nullopt;
    if (right && !PathsEqual(tuPath, right->file))
      return std::nullopt;

    // Prefer the closest side (tie-break to right).
    if (right && (!left || dRight <= dLeft))
      return {{right->b, right->b}};
    if (left)
      return {{left->e, left->e}};

    return std::nullopt;
  }

  // No TU tokens in [a0,a1) (and no safe TU insertion anchor).
  return std::nullopt;
}

const RefoldModel::IncludeItem *
RefoldEngine::BoundaryParentIncludeForPureInsertion(
    const diffutils::Hunk &h) const {
  // Only applicable for insertions (empty A-span). Defensive guard: ignore
  // empty B-span.
  if (h.aStart != h.aEnd || h.bStart >= h.bEnd) {
    return nullptr;
  }

  const uint64_t aPos = h.aStart;

  // Hardened policy (#4): do NOT probe/snap to "nearest" tokmap entries.
  // We only infer an include owner when the insertion lands exactly on an
  // include PP boundary.
  const RefoldModel::IncludeItem *leftBest = nullptr;
  uint64_t leftWidth = std::numeric_limits<uint64_t>::max();

  const RefoldModel::IncludeItem *rightBest = nullptr;
  uint64_t rightWidth = std::numeric_limits<uint64_t>::max();

  for (const auto &inc : model_.GetIncludes()) {
    if (!inc.cover.IsValid())
      continue;

    const uint64_t width = inc.cover.end - inc.cover.begin;

    // Include immediately to the left: ends exactly at aPos.
    if (inc.cover.end == aPos) {
      if (width < leftWidth) {
        leftBest = &inc;
        leftWidth = width;
      }
    }

    // Include immediately to the right: begins exactly at aPos.
    if (inc.cover.begin == aPos) {
      if (width < rightWidth) {
        rightBest = &inc;
        rightWidth = width;
      }
    }
  }

  const std::optional<uint64_t> leftIncId =
      leftBest ? std::optional<uint64_t>(leftBest->id) : std::nullopt;
  const std::optional<uint64_t> rightIncId =
      rightBest ? std::optional<uint64_t>(rightBest->id) : std::nullopt;

  if (!leftIncId && !rightIncId)
    return nullptr; // not at a known include boundary

  const std::optional<uint64_t> parentId =
      model_.LeastCommonAncestorInclude(leftIncId, rightIncId);
  if (!parentId)
    return nullptr;

  const RefoldModel::IncludeItem *parent = model_.GetIncludeById(*parentId);

  trace("include/boundary",
        "BoundaryParentIncludeForPureInsertion: aPos={0} leftInc={1} "
        "rightInc={2} parent={3}",
        aPos, leftIncId, rightIncId, parentId);

  return parent;
}

// ==================== Patch builders (include & macro) ====================

bool RefoldEngine::MacroExpansionEnvelopeB(
    const RefoldModel::MacroInvocation &m, bool onlyInvFile, uint64_t &begin,
    uint64_t &end) const {
  const auto &tokMapByPP = model_.GetTokmapByPP();
  if (tokMapByPP.empty())
    return false;

  uint64_t lo = std::numeric_limits<uint64_t>::max();
  uint64_t hi = 0;
  bool any = false;

  auto addRange = [&](uint64_t l, uint64_t h) {
    for (uint64_t pp = l; pp < h; ++pp) {
      auto it = tokMapByPP.find(pp);
      if (it == tokMapByPP.end())
        continue;

      const auto &t = it->second;

      if (onlyInvFile) {
        if (!m.invFile || !PathsEqual(t.file, *m.invFile))
          continue;
      }

      if (pp < lo)
        lo = pp;
      if (pp + 1 > hi)
        hi = pp + 1;
      any = true;
    }
  };

  // BODY spans
  for (const auto &s : m.bodySpans) {
    if (!s.IsValid())
      continue;
    addRange(s.begin, s.end);
  }

  // ARG spans
  for (const auto &s : m.argSpans) {
    if (!s.IsValid())
      continue;
    addRange(s.begin, s.end);
  }

  if (!any)
    return false;

  begin = lo;
  end = hi;
  return true;
}

bool RefoldEngine::MacroArgReplacementMatchesAllOccurrencesInBImpl(
    const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
    StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks,
    bool checkPasteSpans) const {
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

  if (!hasAny) {
    for (const auto &s : m.pasteSpans) {
      if (s.argIdx == argIdx) {
        hasAny = true;
        break;
      }
    }
  }

  if (!hasAny)
    return true;

  const uint64_t maxTok =
      bTokOff_.empty() ? 0ULL : static_cast<uint64_t>(bTokOff_.size() - 1);
  StringRef argTrim = newArg.trim();
  StringRef baseTrim = baseArg.trim();

  // If this arg is stringified anywhere, accept args-only without enforcing
  // paste-span checks.
  bool argIsStringified = false;
  if (strict_) {
    for (const auto &s : m.stringifySpans) {
      if (s.argIdx == argIdx) {
        argIsStringified = true;
        break;
      }
    }

    // Check all STRINGIFY spans for this argument, but only in strict mode.
    if (argIsStringified) {
      for (const auto &s : m.stringifySpans) {
        if (s.argIdx != argIdx)
          continue;

        auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(s);
        if (!bEnv)
          return false;

        // Extend the B-envelope to account for hunks that touch this
        // occurrence. This is required for insertions at the argument boundary
        // (e.g. appending tokens).
        if (!tokenHunks.empty()) {
          size_t lo = bEnv->first;
          size_t hi = bEnv->second;
          for (const auto &h : tokenHunks) {
            bool touches;
            if (h.aStart == h.aEnd) {
              touches = (h.aStart >= s.begin && h.aStart <= s.end);
            } else {
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

        StringRef tok = SliceBSource(bEnv->first, bEnv->second).trim();
        if (tok.empty())
          return false;

        std::string expect = stringutils::quoteCString(argTrim);
        if (tok != expect) {
          trace("macro/consistency",
                "STRINGIFY mismatch inv id={0} name={1} argIdx={2} tok={3} "
                "expect={4}",
                m.id, m.name, argIdx, tok, expect);
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

    StringRef aTokText = SliceASource(ps.begin, ps.end).trim();
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

    auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(s);
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
        bool touches;
        if (h.aStart == h.aEnd) {
          touches = (h.aStart >= s.begin && h.aStart <= s.end);
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

    StringRef tokText = SliceBSource(bEnv->first, bEnv->second).trim();
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

    auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(ps);
    if (!bEnv || bEnv->second <= bEnv->first ||
        (bEnv->second - bEnv->first) != 1)
      return false;

    StringRef aTokText = SliceASource(ps.begin, ps.end).trim();
    if (aTokText.empty())
      return false;
    StringRef bTokText = SliceBSource(bEnv->first, bEnv->second).trim();
    if (bTokText.empty())
      return false;

    if (!ps.byteBegin || *ps.byteEnd < *ps.byteBegin ||
        static_cast<size_t>(*ps.byteEnd) > aTokText.size())
      return false;

    StringRef oldSeg =
        aTokText.substr(*ps.byteBegin, *ps.byteEnd - *ps.byteBegin);

    auto toSigned = [](std::optional<uint32_t> opt) -> int64_t {
      return static_cast<int64_t>(opt.value_or(0));
    };

    int64_t delta = static_cast<int64_t>(bTokText.size()) -
                    static_cast<int64_t>(aTokText.size());

    int64_t bb = toSigned(ps.byteBegin);
    int64_t be = toSigned(ps.byteEnd) + delta;

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

bool RefoldEngine::HunkTouchesAnyPasteToken(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h) {
  if (m.pasteSpans.empty())
    return false;

  const uint64_t a0 = h.aStart;
  const uint64_t a1 = h.aEnd;

  // Insertion hunk: treat as touching if the insertion point lies "on" a paste
  // span boundary.
  if (a0 == a1) {
    for (const auto &s : m.pasteSpans) {
      if (a0 >= s.begin && a0 <= s.end)
        return true;
    }
    return false;
  }

  // Replacement/deletion hunk: interval intersection between [a0, a1) and
  // [s.begin, s.end).
  for (const auto &s : m.pasteSpans) {
    if (a0 < s.end && a1 > s.begin)
      return true;
  }

  return false;
}

std::optional<RefoldEngine::PasteArgEdit>
RefoldEngine::DerivePasteArgEdit(const RefoldModel::MacroInvocation &m,
                                 const diffutils::Hunk &h) const {
  // This helper only applies when the producer reported paste spans.
  if (m.pasteSpans.empty())
    return std::nullopt;

  // Collect all paste span occurrences that intersect the hunk in A-token
  // space.
  //
  // Each PPArgSpan in pasteSpans corresponds to one argument's contribution to
  // a single pasted token emitted in A_PP. Multiple arguments may contribute
  // disjoint (or adjacent) byte segments inside the same pasted token, and the
  // hunk may touch one of those segments.
  std::vector<const RefoldModel::PPArgSpan *> cands;
  for (const auto &ps : m.pasteSpans) {
    if (ps.begin < h.aEnd && h.aStart < ps.end)
      cands.push_back(&ps);
  }

  if (cands.empty())
    return std::nullopt;

  // All candidates must refer to the same pasted-token occurrence. In practice,
  // each candidate has the same [begin,end) A-token envelope (the pasted
  // token), but different argIdx and [byteBegin,byteEnd) describing which byte
  // subrange of the pasted token came from that argument.
  const auto *tokenSpan = cands[0];

  // Map the pasted token envelope in A to its corresponding envelope in B. For
  // paste edits we require a strict mapping: the A pasted token must map to
  // exactly one B token that we will diff against.
  auto bEnvOpt = MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
  if (!bEnvOpt || bEnvOpt->second <= bEnvOpt->first)
    return std::nullopt;

  // Paste-aware edits handled here must stay within a single B token. If the
  // pasted token turned into multiple tokens in B, the edit is not a pure
  // within-token paste segment rewrite, so bail out.
  if (bEnvOpt->second - bEnvOpt->first != 1)
    return std::nullopt;

  // Grab the raw token spellings for the pasted token in A and B.
  // tokenSpan.begin/end are A-token indices; bEnv[0]/bEnv[1] are B-token
  // indices.
  StringRef aTokRaw = SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw = SliceBSource(bEnvOpt->first, bEnvOpt->second);

  // Strip trailing newlines to stabilize within-token diffs.
  auto stripTrailingNL = [](StringRef s) {
    while (s.ends_with("\n"))
      s = s.drop_back();
    return s;
  };

  StringRef aTok = stripTrailingNL(aTokRaw);
  StringRef bTok = stripTrailingNL(bTokRaw);

  // Compute the minimal differing region between the two token spellings:
  // aTok = [common prefix][DIFF_A][common suffix]
  // bTok = [common prefix][DIFF_B][common suffix]
  size_t pref = 0;
  size_t minLen = std::min(aTok.size(), bTok.size());
  while (pref < minLen && aTok[pref] == bTok[pref]) {
    pref++;
  }

  size_t aLen = aTok.size();
  size_t bLen = bTok.size();
  size_t suff = 0;

  // While we haven't reached the prefix on either side
  // and the characters from the back match...
  while (suff < (aLen - pref) && suff < (bLen - pref) &&
         aTok[aLen - 1 - suff] == bTok[bLen - 1 - suff]) {
    suff++;
  }

  const size_t diffStart = pref;
  const size_t diffEndA = aLen - suff;

  // If there is no difference at all, this hunk cannot be explained as a
  // paste-segment rewrite.
  if (diffStart >= diffEndA && aTok.size() == bTok.size())
    return std::nullopt;

  // Now choose exactly one candidate argument contribution whose
  // [byteBegin,byteEnd) overlaps the differing region. The producer provided
  // byteBegin/byteEnd in pasted-token text coordinates.
  //
  // We require the edit to be attributable to a single argument slice. If
  // multiple slices overlap the diff region, we cannot express it as a
  // single-arg args-only rewrite.
  const RefoldModel::PPArgSpan *chosen = nullptr;
  for (const auto *ps : cands) {
    if (!ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin)
      continue;

    const size_t bBegin = *ps->byteBegin;
    const size_t bEnd = *ps->byteEnd;

    bool hit = false;
    if (diffStart == diffEndA) {
      // Pure insertion/deletion at a point (no width in A). Treat as
      // overlapping if the point lies strictly inside the candidate slice.
      hit = (bBegin <= diffStart) && (diffStart < bEnd);
    } else {
      // General overlap between [diffStart,diffEndA) and
      // [ps.byteBegin,ps.byteEnd).
      const size_t lo = std::max(bBegin, diffStart);
      const size_t hi = std::min(bEnd, diffEndA);
      hit = (hi > lo);
    }

    if (hit) {
      if (!chosen)
        chosen = ps;
      else
        return std::nullopt; // Overlaps multiple args.
    }
  }

  if (!chosen)
    return std::nullopt;

  // Extract the old contributed segment from the A pasted token.
  const int64_t bbA = static_cast<int64_t>(*chosen->byteBegin);
  const int64_t beA = static_cast<int64_t>(*chosen->byteEnd);

  if (bbA < 0 || beA < bbA || static_cast<uint64_t>(beA) > aTok.size())
    return std::nullopt;

  // Compute the corresponding segment coordinates in the B pasted token.
  //
  // We assume the token-level edit does not permute the contribution
  // boundaries; instead, the chosen segment grows/shrinks by the overall token
  // length delta (bTokLen - aTokLen). This allows us to map [bb,be) in A to
  // [bb,be+delta) in B.
  const int64_t delta =
      static_cast<int64_t>(bTok.size()) - static_cast<int64_t>(aTok.size());
  const int64_t bbB_signed = bbA; // Assumption: prefix is stable
  const int64_t beB_signed = beA + delta;
  if (bbB_signed < 0 || beB_signed < bbB_signed ||
      static_cast<uint64_t>(beB_signed) > bTok.size())
    return std::nullopt;

  const size_t bb = static_cast<size_t>(bbA);
  const size_t be = static_cast<size_t>(beA);
  const size_t bbB = static_cast<size_t>(bbB_signed);
  const size_t beB = static_cast<size_t>(beB_signed);

  // Safety gate: ensure the only edits to the pasted token are within the
  // chosen segment.
  //
  // This requires both:
  // - the prefix before bb matches exactly
  // - the suffix after be matches exactly (after shifting by delta in B)
  if (aTok.substr(0, bb) != bTok.substr(0, bbB))
    return std::nullopt;

  if (aTok.substr(be) != bTok.substr(beB))
    return std::nullopt;

  std::string oldSeg = aTok.substr(bb, be - bb).str();
  std::string newSeg = bTok.substr(bbB, beB - bbB).str();

  return PasteArgEdit(chosen->argIdx, std::move(newSeg), std::move(oldSeg));
}

std::optional<std::vector<RefoldEngine::PasteArgEdit>>
RefoldEngine::DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                                  const diffutils::Hunk &h) const {
  if (m.pasteSpans.empty())
    return std::nullopt;

  // Gather all paste spans that intersect this hunk in the A-stream.
  std::vector<const RefoldModel::PPArgSpan *> cands;
  for (const auto &ps : m.pasteSpans) {
    if (ps.begin < h.aEnd && h.aStart < ps.end)
      cands.push_back(&ps);
  }

  if (cands.empty())
    return std::nullopt;

  // All candidates should reference the same pasted token range [begin, end) in
  // A.
  const auto *tokenSpan = cands[0];

  auto bEnvOpt = MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
  if (!bEnvOpt || bEnvOpt->second <= bEnvOpt->first)
    return std::nullopt;

  // Paste edits are only representable as args-only when the A-span maps to
  // exactly one B token.
  if (bEnvOpt->second - bEnvOpt->first != 1)
    return std::nullopt;

  StringRef aTokRaw = SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw = SliceBSource(bEnvOpt->first, bEnvOpt->second);

  // Strip trailing newlines using StringRef for efficiency.
  auto stripTrailingNL = [](StringRef s) {
    while (s.ends_with("\n"))
      s = s.drop_back();
    return s;
  };

  StringRef aTok = stripTrailingNL(aTokRaw);
  StringRef bTok = stripTrailingNL(bTokRaw);

  // Multi-span paste edits may change the overall pasted token length (e.g.,
  // a_b_c -> foo_bar_baz). This is still safe to refold *as long as* the
  // non-arg "fixed" slices of the pasted token remain unchanged, and we can
  // deterministically segment the B token into the per-arg regions.
  //
  // We derive the new per-arg segments by walking the A token left-to-right and
  // using the fixed (non-span) substrings between paste spans as anchors. If
  // spans are adjacent (no fixed anchor) and the total length changes,
  // segmentation is ambiguous and we conservatively return null.
  std::vector<const RefoldModel::PPArgSpan *> spans = cands;
  std::sort(spans.begin(), spans.end(), [](const auto *p1, const auto *p2) {
    // If p1 has no value, it's "greater" than anything with a value (moves to
    // end)
    if (!p1->byteBegin)
      return false;
    if (!p2->byteBegin)
      return true;

    // If both have values, compare them
    if (*p1->byteBegin != *p2->byteBegin)
      return *p1->byteBegin < *p2->byteBegin;

    // Stable tie-breaker: sort by end position if starts are equal
    uint32_t end1 = p1->byteEnd.value_or(0);
    uint32_t end2 = p2->byteEnd.value_or(0);
    return end1 < end2;
  });

  std::optional<std::vector<std::string>> newSegs =
      SegmentPastedTokenArgsByFixedSlices(aTok, bTok, spans);
  if (!newSegs)
    return std::nullopt;

  std::vector<PasteArgEdit> edits;

  for (size_t i = 0; i < spans.size(); ++i) {
    const auto *ps = spans[i];
    if (!ps->byteBegin || *ps->byteEnd < *ps->byteBegin)
      return std::nullopt;

    size_t bb = static_cast<size_t>(*ps->byteBegin);
    size_t be = static_cast<size_t>(*ps->byteEnd);
    if (be > aTok.size())
      return std::nullopt;

    StringRef oldSeg = aTok.substr(bb, be - bb);
    const std::string &newSeg = (*newSegs)[i];

    if (oldSeg == newSeg)
      continue;

    // Note: the same argument may contribute multiple segments to the same
    // pasted token (e.g. X##_..._##X). We allow repeated argIdx here and let
    // the caller merge implied argument replacements conservatively.
    edits.emplace_back(ps->argIdx, newSeg, oldSeg.str());
  }

  if (edits.empty())
    return std::nullopt;

  return edits;
}

std::optional<std::vector<std::string>>
RefoldEngine::SegmentPastedTokenArgsByFixedSlices(
    StringRef aTok, StringRef bTok,
    ArrayRef<const RefoldModel::PPArgSpan *> spansAsc) {
  if (spansAsc.empty())
    return std::nullopt;

  // Basic span sanity.
  for (const auto *ps : spansAsc) {
    if (!ps->byteBegin || *ps->byteEnd < *ps->byteBegin)
      return std::nullopt;
    if (static_cast<size_t>(*ps->byteEnd) > aTok.size())
      return std::nullopt;
  }

  // Initialize the output vector with empty strings.
  std::vector<std::string> out(spansAsc.size());

  // Call the recursive worker.
  if (!SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc,
                                              /*idx*/ 0, /*posA*/ 0,
                                              /*posB*/ 0, out)) {
    return std::nullopt;
  }

  return out;
}

bool RefoldEngine::SegmentPastedTokenArgsByFixedSlicesRec(
    StringRef aTok, StringRef bTok,
    ArrayRef<const RefoldModel::PPArgSpan *> spansAsc, size_t idx, size_t posA,
    size_t posB, MutableArrayRef<std::string> out) {
  // 1. Base Case: All spans processed
  if (idx >= spansAsc.size()) {
    // All spans emitted; remaining fixed tail must match exactly.
    StringRef tail = aTok.substr(posA);
    return bTok.substr(posB) == tail;
  }

  const auto *ps = spansAsc[idx];

  // 2. Validate Optionals and Range Consistency
  if (!ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin)
    return false;

  const size_t bA = static_cast<size_t>(*ps->byteBegin);
  const size_t eA = static_cast<size_t>(*ps->byteEnd);

  // Ensure current span doesn't overlap backwards into previously processed A
  // text
  if (bA < posA)
    return false;

  // 3. Match the "Fixed" anchor text appearing before this argument in A
  StringRef fixedBefore = aTok.substr(posA, bA - posA);
  if (!bTok.substr(posB).starts_with(fixedBefore))
    return false;

  const size_t argStartB = posB + fixedBefore.size();
  const size_t nextPosA = eA;

  // 4. Determine the "Fixed" anchor text appearing after this argument
  StringRef fixedAfter;
  if (idx + 1 < spansAsc.size()) {
    const auto *next = spansAsc[idx + 1];
    if (!next->byteBegin || *next->byteBegin < eA)
      return false;
    fixedAfter = aTok.substr(eA, static_cast<size_t>(*next->byteBegin) - eA);
  } else {
    fixedAfter = aTok.substr(eA);
  }

  // 5. Handling Argument Boundaries
  if (fixedAfter.empty()) {
    // If there is no fixed anchor after this span and the total length changed,
    // we cannot determine the B segment boundary for this arg.
    if (idx + 1 < spansAsc.size() && aTok.size() != bTok.size())
      return false;

    // If this is the final arg span and there is no fixed tail, the arg's
    // contribution may legally grow or shrink. In that case, consume the
    // remainder of the B token.
    //
    // This is the common case for token-paste macros like:
    //   CONCAT(X, Y, Z) -> X##_##Y##_##Z
    // where the last argument is immediately followed by the end of the pasted
    // token.
    if (idx + 1 == spansAsc.size()) {
      out[idx] = bTok.substr(argStartB).str();
      return SegmentPastedTokenArgsByFixedSlicesRec(
          aTok, bTok, spansAsc, idx + 1, nextPosA, bTok.size(), out);
    }

    // Length-stable adjacent spans: use the A span length as the B span length.
    const size_t aLen = eA - bA;
    const size_t argEndB = argStartB + aLen;

    if (argEndB > bTok.size())
      return false;

    out[idx] = bTok.substr(argStartB, aLen).str();
    return SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc, idx + 1,
                                                  nextPosA, argEndB, out);
  }

  // 6. Anchor Search: Find where the fixedAfter text appears in B
  // We search starting at argStartB.
  size_t k = bTok.find(fixedAfter, argStartB);
  while (k != StringRef::npos) {
    // Current candidate for the argument content in B
    out[idx] = bTok.substr(argStartB, k - argStartB).str();

    // Recurse to see if this candidate allows the rest of the string to match
    if (SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc, idx + 1,
                                               nextPosA, k, out)) {
      return true;
    }

    // Backtrack: Find the next occurrence of the anchor
    k = bTok.find(fixedAfter, k + 1);
  }

  return false;
}

StringRef RefoldEngine::DeriveNewPasteSegmentFromSpellingReplacement(
    StringRef baseArg, StringRef newArg, StringRef oldSeg) {
  // Trim all inputs.
  baseArg = baseArg.trim();
  newArg = newArg.trim();
  oldSeg = oldSeg.trim();

  // If the segment is the entire argument, the replacement is the entire new
  // argument.
  if (oldSeg == baseArg)
    return newArg;

  // Case 1: oldSeg is a prefix of baseArg.
  // Example: base="foo_v1", oldSeg="foo_", new="bar_v1" -> returns "bar"
  if (baseArg.starts_with(oldSeg)) {
    StringRef suffix = baseArg.substr(oldSeg.size());
    if (!newArg.ends_with(suffix))
      return StringRef();

    // Return the part of newArg that precedes the suffix.
    return newArg.substr(0, newArg.size() - suffix.size());
  }

  // Case 2: oldSeg is a suffix of baseArg.
  // Example: base="v1_foo", oldSeg="_foo", new="v1_bar" -> returns "bar"
  if (baseArg.ends_with(oldSeg)) {
    StringRef prefix = baseArg.substr(0, baseArg.size() - oldSeg.size());
    if (!newArg.starts_with(prefix))
      return StringRef();

    // Return the part of newArg that follows the prefix.
    return newArg.substr(prefix.size());
  }

  return StringRef();
}

bool RefoldEngine::PasteArgReplacementsMatchAllPasteTokensInB(
    const RefoldModel::MacroInvocation &m, StringRef baseInvText,
    ArrayRef<std::pair<size_t, size_t>> invArgRanges,
    const DenseMap<uint32_t, std::string> &replByArgIdx) const {
  if (m.pasteSpans.empty())
    return true;

  // Precompute the original (base) spelling text for each argument we are
  // proposing to replace. We need this to derive a stable mapping from
  // "argument replacement" -> "paste segment update".
  DenseMap<uint32_t, std::string> baseArgByIdx;
  for (const auto &entry : replByArgIdx) {
    uint32_t argIdx = entry.first;
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return false;

    auto range = invArgRanges[argIdx];
    StringRef rawArg =
        baseInvText.substr(range.first, range.second - range.first);
    baseArgByIdx[argIdx] = rawArg.trim().str();
  }

  // Group paste spans by the specific pasted-token occurrence they contribute
  // to. The grouping key is the A token interval [beginTok,endTok) of the
  // pasted token. In practice, paste spans are expected to describe a single
  // token, so (endTok - beginTok) should be 1.
  std::vector<std::pair<uint64_t, uint64_t>> tokenOrder;
  DenseMap<std::pair<uint64_t, uint64_t>, std::vector<RefoldModel::PPArgSpan>>
      spansByTok;
  for (const auto &ps : m.pasteSpans) {
    std::pair<uint64_t, uint64_t> key = {ps.begin, ps.end};
    if (spansByTok.find(key) == spansByTok.end()) {
      tokenOrder.push_back(key);
    }
    spansByTok[key].push_back(ps);
  }

  // For each pasted-token occurrence, simulate applying the per-arg
  // replacements to its sub-token argument segments and compare against the
  // edited B token spelling.
  for (const auto &key : tokenOrder) {
    uint64_t beginTok = key.first;
    uint64_t endTok = key.second;

    // We only support pasted-token occurrences that correspond to exactly one
    // token in A.
    if (endTok != beginTok + 1)
      return false;

    // Map the A pasted-token occurrence to a single B token envelope.
    auto bEnv = MapATokRangeAToBTokenEnvelope(beginTok, endTok);
    if (!bEnv || bEnv->second != bEnv->first + 1)
      return false;

    auto stripNL = [](StringRef s) -> std::string {
      std::string result = s.str(); // Copy StringRef to a mutable string
      llvm::erase_if(result, [](char c) { return c == '\n'; });
      return result;
    };

    // Extract the pasted token text as produced in A and B. We strip newlines
    // defensively since slice helpers may include trailing '\n' depending on
    // how token ranges were formed.
    std::string aTok = stripNL(SliceASource(beginTok, endTok));
    std::string bTok = stripNL(SliceBSource(bEnv->first, bEnv->second));

    // Paste spans for this token reference character slices inside the pasted
    // token spelling. Apply edits in descending byteBegin so earlier rewrites
    // do not shift later offsets.
    std::vector<RefoldModel::PPArgSpan> &spans = spansByTok[key];

    // Apply in descending byteBegin so replacements cannot shift the offsets
    // of later spans.
    std::sort(spans.begin(), spans.end(), [](const auto &p1, const auto &p2) {
      // Spans without byteBegin are treated as "last".
      if (!p1.byteBegin)
        return false;
      if (!p2.byteBegin)
        return true;

      // Primary key: descending start.
      if (*p1.byteBegin != *p2.byteBegin)
        return *p1.byteBegin > *p2.byteBegin;

      // Tie-breaker: descending end.
      uint32_t end1 = p1.byteEnd.value_or(0);
      uint32_t end2 = p2.byteEnd.value_or(0);
      return end1 > end2;
    });

    // Start from the A token spelling and simulate the token-paste result after
    // applying the candidate arg replacements.
    std::string expected = aTok;
    for (const auto &ps : spans) {
      // Only apply span updates for arguments that we are actively replacing.
      auto it = replByArgIdx.find(ps.argIdx);
      if (it == replByArgIdx.end())
        continue;
      StringRef newArg = it->second;

      // The segment derivation also needs the original spelling of the
      // argument.
      auto baseIt = baseArgByIdx.find(ps.argIdx);
      if (baseIt == baseArgByIdx.end())
        return false;
      StringRef baseArg = baseIt->second;

      if (!ps.byteBegin || !ps.byteEnd)
        return false;

      size_t b = static_cast<size_t>(*ps.byteBegin);
      size_t e = static_cast<size_t>(*ps.byteEnd);

      // The paste span must define a valid character slice inside the A
      // pasted-token spelling.
      if (e < b || e > aTok.size())
        return false;

      // Extract the original pasted-token segment contributed by this argument.
      StringRef oldSeg = StringRef(aTok).substr(b, e - b);

      // Derive the new pasted-token segment from the argument replacement. This
      // is intentionally conservative and must be deterministic; if we cannot
      // derive a segment safely, fail.
      StringRef newSeg =
          DeriveNewPasteSegmentFromSpellingReplacement(baseArg, newArg, oldSeg);
      if (newSeg.data() == nullptr) // Check for "null" StringRef
        return false;

      // Rewrite only the identified segment region inside the synthetic pasted-
      // token spelling.
      expected = stringutils::replaceRange(expected, b, e, newSeg);
    }

    if (expected != bTok)
      return false;
  }

  return true;
}

std::string RefoldEngine::SplicePasteSegmentIntoSpellingArg(StringRef baseArg,
                                                            StringRef oldSeg,
                                                            StringRef newSeg) {
  StringRef baseTrim = baseArg.trim();
  StringRef oldTrim = oldSeg.trim();
  StringRef newTrim = newSeg.trim();

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

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldEngine::GetMacroInvocationFormalArgContentRanges(
    const RefoldModel::MacroInvocation &m, StringRef invText) {
  // Prefer the producer-provided per-formal invocation-argument ranges when
  // available. This is required for variadic macros where multiple "actual"
  // arguments correspond to a single formal (e.g. __VA_ARGS__).
  if (!m.invArgRanges.empty()) {
    // The producer-provided ranges are absolute byte offsets in the invocation
    // file, captured from the *original* invocation spelling. If we've already
    // applied an earlier args-only patch to this invocation (so invText differs
    // from m.invText), those absolute endpoints no longer line up. Re-parse
    // current invocation spelling and then re-map to the producer's formal
    // arity (including the variadic tail) to keep subsequent hunks stable.
    if (m.invText && invText != *m.invText) {
      auto parsedOpt =
          RefoldEngine::ParseMacroInvocationArgContentRanges(invText);
      if (!parsedOpt)
        return std::nullopt;

      const auto &parsed = *parsedOpt;
      const size_t formalN = m.invArgRanges.size();
      const size_t actualN = parsed.size();

      // Helper to return a safe "empty" range at the close-paren location.
      auto emptyAtCloseParen = [&]() -> std::pair<size_t, size_t> {
        size_t closeIdx = invText.rfind(')');
        if (closeIdx == StringRef::npos)
          closeIdx = invText.size();
        return {closeIdx, closeIdx};
      };

      if (actualN == formalN)
        return parsed;

      std::vector<std::pair<size_t, size_t>> out;
      out.reserve(formalN);

      if (actualN > formalN && formalN > 0) {
        // Variadic call: map the prefix 1:1, and let the last formal span the
        // entire remaining "tail" (including commas/whitespace between args).
        out.insert(out.end(), parsed.begin(), parsed.begin() + (formalN - 1));
        out.push_back({parsed[formalN - 1].first, parsed.back().second});
        return out;
      }

      // actualN < formalN: missing trailing actuals (e.g. empty __VA_ARGS__).
      out.insert(out.end(), parsed.begin(), parsed.end());
      for (size_t i = actualN; i < formalN; ++i)
        out.push_back(emptyAtCloseParen());
      return out;
    }

    if (!m.invB)
      return std::nullopt;

    const uint64_t invB = *m.invB;

    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(m.invArgRanges.size());

    bool anyInvalid = false;
    for (const auto &R : m.invArgRanges) {
      if (!R.first || !R.second) {
        anyInvalid = true;
        out.emplace_back(static_cast<size_t>(-1), static_cast<size_t>(-1));
        continue;
      }

      if (*R.first < invB || *R.second < *R.first) {
        anyInvalid = true;
        out.emplace_back(static_cast<size_t>(-1), static_cast<size_t>(-1));
        continue;
      }

      const uint64_t relB64 = *R.first - invB;
      const uint64_t relE64 = *R.second - invB;

      if (relE64 > invText.size() || relB64 > relE64) {
        anyInvalid = true;
        out.emplace_back(static_cast<size_t>(-1), static_cast<size_t>(-1));
        continue;
      }

      out.emplace_back(static_cast<size_t>(relB64), static_cast<size_t>(relE64));
    }

    if (!anyInvalid)
      return out;

    // Try to fill any invalid entries using a conservative textual parse of the
    // invocation spelling, preserving the formal-parameter indexing when the
    // parsed arity differs (variadics / missing variadic tail).
    auto parsedOpt = RefoldEngine::ParseMacroInvocationArgContentRanges(invText);
    if (!parsedOpt)
      return std::nullopt;

    const auto &parsed = *parsedOpt;
    const size_t formalN = out.size();
    const size_t actualN = parsed.size();

    // Helper to return a safe "empty" range at the close-paren location.
    auto emptyAtCloseParen = [&]() -> std::pair<size_t, size_t> {
      size_t closeIdx = invText.rfind(')');
      if (closeIdx == StringRef::npos)
        closeIdx = invText.size();
      return {closeIdx, closeIdx};
    };

    if (actualN == formalN) {
      for (size_t i = 0; i < formalN; ++i) {
        if (out[i].first == static_cast<size_t>(-1))
          out[i] = parsed[i];
      }
      return out;
    }

    if (actualN > formalN && formalN > 0) {
      // Variadic call: map the prefix 1:1, and let the last formal span the
      // entire remaining "tail" (including commas/whitespace between args).
      for (size_t i = 0; i + 1 < formalN; ++i) {
        if (out[i].first == static_cast<size_t>(-1))
          out[i] = parsed[i];
      }

      if (out[formalN - 1].first == static_cast<size_t>(-1)) {
        out[formalN - 1] = {parsed[formalN - 1].first, parsed.back().second};
      }
      return out;
    }

    // actualN < formalN: missing trailing actuals (e.g. empty __VA_ARGS__).
    for (size_t i = 0; i < std::min(formalN, actualN); ++i) {
      if (out[i].first == static_cast<size_t>(-1))
        out[i] = parsed[i];
    }
    for (size_t i = actualN; i < formalN; ++i) {
      if (out[i].first == static_cast<size_t>(-1))
        out[i] = emptyAtCloseParen();
    }
    return out;
  }

  // Fallback: derive ranges from the invocation spelling. This path assumes a
  // 1:1 mapping between argument index and "actual" arguments.
  return RefoldEngine::ParseMacroInvocationArgContentRanges(invText);
}

std::optional<RefoldEngine::MacroPatch>
RefoldEngine::BuildMacroInvocationPatchArgsOnly(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText) const {
  // We can only emit an invocation patch if the producer provided a concrete
  // byte range.
  if (!m.invB || !m.invE)
    return std::nullopt;

  // tokenHunks are used by macroArgReplacementMatchesAllOccurrencesInB() to
  // validate cross-occurrence consistency. In this args-only path we only need
  // visibility into the current hunk, so treat it as the only token-level edit.
  diffutils::Hunk hArgs = h;

  // Normalize “comma drift” in comma-separated lists (notably for prepending
  // into variadic tails). Token diff can treat the first separator comma as
  // inserted and match a later comma, shifting the insertion left. Rotate the
  // comma back into the match so the insertion is attributed to the next arg.
  if (hArgs.aStart == hArgs.aEnd && hArgs.bStart < hArgs.bEnd &&
      hArgs.aStart < aToks_.size() &&
      hArgs.bStart < bToks_.size() &&
      hArgs.bEnd < bToks_.size() &&
      aToks_[static_cast<size_t>(hArgs.aStart)].spelling == "," &&
      bToks_[static_cast<size_t>(hArgs.bStart)].spelling == "," &&
      bToks_[static_cast<size_t>(hArgs.bEnd)].spelling == "," &&
      hArgs.aStart + 1 <= aToks_.size() &&
      hArgs.bStart + 1 <= bToks_.size() &&
      hArgs.bEnd + 1 <= bToks_.size()) {
    ++hArgs.aStart;
    ++hArgs.aEnd;
    ++hArgs.bStart;
    ++hArgs.bEnd;
  }

  const diffutils::Hunk tokenHunks[] = {hArgs};

  trace("macro/args", "args-only? inv id={0} name={1} {2} baseInv={3}", m.id,
        m.name, h, stringutils::showWSWithClip(baseInvText, 200));

  // Parse the byte ranges for each argument's "content" within the invocation
  // spelling. These ranges are later used to splice per-arg replacements back
  // into the invocation text.
  auto rangesOpt = GetMacroInvocationFormalArgContentRanges(m, baseInvText);
  if (!rangesOpt)
    return std::nullopt;
  const auto &invArgRanges = *rangesOpt;

  trace("macro/args", "  invArgRanges(%d)=%s", invArgRanges.size(),
        stringutils::rangesToStringWithSlices(baseInvText, invArgRanges));

  // Fast path for token-paste edits. A single pasted token can embed multiple
  // argument contributions (e.g., X##_##Y##_##Z), so a single edit hunk may
  // change multiple arg segments inside that token (e.g., a_b_c -> d_e_f). In
  // that case we attempt to derive per-arg segment replacements and splice them
  // into the invocation spelling.
  if (HunkTouchesAnyPasteToken(m, h)) {
    auto edits = DerivePasteArgEdits(m, h);
    if (edits && !edits->empty()) {
      DenseMap<uint32_t, std::string> replByArgIdx;
      for (const auto &pae : *edits) {
        uint32_t argIdx = pae.argIdx;
        if (static_cast<size_t>(argIdx) >= invArgRanges.size())
          return std::nullopt;

        // A single argument may contribute multiple segments to the same
        // pasted token (e.g. X##_..._##X). We merge repeated argIdx
        // conservatively after deriving the candidate replacement below.
        auto range = invArgRanges[argIdx];
        StringRef baseArgText =
            baseInvText.substr(range.first, range.second - range.first);

        // Splice the sub-token replacement into the spelling arg
        // conservatively.
        std::string newArg = SplicePasteSegmentIntoSpellingArg(
            baseArgText, pae.oldSeg, pae.newSeg);
        if (newArg.empty()) {
          // Deleting an entire argument (making it empty) is legal. Accept this only
          // when the paste-span covered the whole argument spelling.
          if (!(StringRef(pae.newSeg).trim().empty() &&
                baseArgText.trim() == StringRef(pae.oldSeg).trim()))
            return std::nullopt;
        }

        auto existing = replByArgIdx.find(argIdx);
        if (existing != replByArgIdx.end()) {
          if (existing->second != newArg)
            return std::nullopt;
          continue;
        }

        // Per-arg safety gate: validate standard + stringify occurrences for
        // this arg.
        //
        // NOTE: For multi-span paste edits where the pasted token length may
        // change, per-arg paste-span validation cannot be done reliably in
        // isolation. We validate paste tokens as a *group* below via
        // pasteArgReplacementsMatchAllPasteTokensInB(...).
        if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
                m, argIdx, baseArgText, newArg, tokenHunks)) {
          return std::nullopt;
        }

        replByArgIdx[argIdx] = std::move(newArg);
      }

      if (!replByArgIdx.empty()) {
        // Combined safety gate: applying all derived replacements must
        // reconstruct every pasted token occurrence exactly as seen in B.
        if (!PasteArgReplacementsMatchAllPasteTokensInB(
                m, baseInvText, invArgRanges, replByArgIdx)) {
          return std::nullopt;
        }

        // Apply all replacements to the invocation string (descending order).
        std::string newInv = baseInvText.str();
        auto keys = llvm::to_vector<8>(
            llvm::map_range(replByArgIdx, [](auto &e) { return e.first; }));
        std::sort(keys.begin(), keys.end(), [&](uint32_t a, uint32_t b) {
          return invArgRanges[a].first > invArgRanges[b].first;
        });

        for (uint32_t argIdx : keys) {
          auto r = invArgRanges[argIdx];
          newInv = stringutils::replaceRange(newInv, r.first, r.second,
                                             replByArgIdx[argIdx]);
        }

        trace("macro/args", "  args-only SUCCESS newInv='{0}'",
              stringutils::showWSWithClip(newInv, 200));
        return MacroPatch{*m.invB, *m.invE, std::move(newInv)};
      }
    }

    // Single-segment paste edit (existing behavior)
    //
    // This handles the common case where only one pasted segment changes (e.g.
    // X##_##Y, changing just X). The multi-span derivation above requires token
    // lengths to remain stable; when they do not, we fall back to deriving a
    // single segment edit from the token-level diff.
    auto pae = DerivePasteArgEdit(m, h);
    if (pae) {
      uint32_t argIdx = pae->argIdx;

      // HARD FAILURE: If we derived a paste edit but the index is invalid,
      // we must exit, not fall through.
      if (static_cast<size_t>(argIdx) >= invArgRanges.size())
        return std::nullopt;

      auto r = invArgRanges[argIdx];
      StringRef baseArgText = baseInvText.substr(r.first, r.second - r.first);
      std::string newArg = SplicePasteSegmentIntoSpellingArg(
          baseArgText, pae->oldSeg, pae->newSeg);
      if (newArg.empty()) {
        // Deleting an entire argument (making it empty) is legal. Accept this only
        // when the paste-span covered the whole argument spelling.
        if (!(StringRef(pae->newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae->oldSeg).trim()))
          return std::nullopt;
      }

      // Safety gate: for single-segment paste edits we can directly validate
      // all occurrences, including paste-span occurrences, against the B
      // stream.
      if (!MacroArgReplacementMatchesAllOccurrencesInB(m, argIdx, baseArgText,
                                                       newArg, tokenHunks)) {
        return std::nullopt;
      }

      std::string newInv =
          stringutils::replaceRange(baseInvText, r.first, r.second, newArg);
      trace("macro/args", "  args-only SUCCESS newInv='{0}'",
            stringutils::showWSWithClip(newInv, 200));
      return MacroPatch{*m.invB, *m.invE, std::move(newInv)};
    }

    // If we touched paste but could not safely derive a paste splice patch,
    // fall through to the standard (non-paste) args-only policy below.
  }

  // Standard (non-paste) args-only policy:
  // Collect arg-span occurrences (and stringify occurrences) and require the
  // entire hunk to be covered by those spans. Then derive per-arg replacements
  // from the B slices.
  std::vector<RefoldModel::PPArgSpan> occs;
  append_range(occs, m.argSpans);
  append_range(occs, m.stringifySpans);

  std::vector<char> occIsStringify;
  occIsStringify.resize(occs.size());
  std::fill_n(occIsStringify.begin(), m.argSpans.size(), false);
  std::fill_n(occIsStringify.begin() + m.argSpans.size(),
              m.stringifySpans.size(), true);

  trace("macro/args", "  occs={0}",
        PPArgSpanListToString(occs, occIsStringify));
  if (occs.empty())
    return std::nullopt;

  std::vector<char> touched(occs.size(), 0);
  if (!HunkFullyWithinArgSpans(hArgs, occs, touched)) {
    trace("macro/args",
          "  hunk not fully within any arg spans -> fail args-only");
    return std::nullopt;
  }

  trace("macro/args", "  touched={0}", stringutils::boolArrayToString(touched));

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.
  DenseMap<uint32_t, std::string> replByArgIdx;
  for (size_t i = 0; i < occs.size(); ++i) {
    if (!touched[i])
      continue;

    const auto &sp = occs[i];
    uint32_t argIdx = sp.argIdx;
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return std::nullopt;

    // Base spelling for this argument in the invocation text (used for splice
    // and consistency).
    auto r0 = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r0.first, r0.second - r0.first);

    // Map the A occurrence envelope to B using byte-level hunks derived from
    // the global alignment. This leverages producer-provided pp_byte_begin/
    // pp_byte_end when present and falls back to consumer token offsets
    // otherwise.
    auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(sp);
    if (!bEnv) {
      // Conservative fallback: use the hunk's B range if the span mapping
      // fails. If the hunk doesn't have a valid B range either, we cannot
      // safely derive an args-only replacement.
      if (h.bStart >= h.bEnd)
        return std::nullopt;
      bEnv = {static_cast<size_t>(h.bStart), static_cast<size_t>(h.bEnd)};
    }

    // Slice the edited text from B corresponding to this occurrence and treat
    // it as the candidate replacement for the argument (subject to stringify
    // decoding and paste lifting below).
    StringRef bSlice = SliceBSource(bEnv->first, bEnv->second).trim();
    std::string newArg = bSlice.str();

    // For stringify occurrences, the B slice is a string literal; decode it
    // back into the argument text that would produce that literal via string-
    // ification.
    if (occIsStringify[i]) {
      auto un = UnstringifyLiteralToArgText(bSlice);
      if (!un)
        return std::nullopt;
      newArg = std::move(*un);
    }

    // Optional lift/paste rewrite: when an argument participates in token
    // pasting, the direct B slice might reflect only the pasted contribution
    // rather than the full argument spelling. In that case, attempt to replace
    // only the A occurrence slice within the base argument spelling.
    if (!occIsStringify[i] && !m.pasteSpans.empty()) {
      bool argHasPaste = false;
      for (const auto &ps : m.pasteSpans) {
        if (ps.argIdx == argIdx) {
          argHasPaste = true;
          break;
        }
      }

      if (argHasPaste) {
        StringRef aSlice = SliceASource(sp.begin, sp.end).trim();
        if (!aSlice.empty()) {
          size_t pos = baseArgText.find(aSlice);
          if (pos != StringRef::npos) {
            std::string cand = baseArgText.substr(0, pos).str() + bSlice.str() +
                               baseArgText.substr(pos + aSlice.size()).str();
            newArg = StringRef(cand).trim().str();
            trace("macro/args",
                  "    lift/paste argIdx={0} baseArg={1} aSlice={2} bSlice={3} "
                  "-> newArg={4}",
                  argIdx, stringutils::showWSWithClip(baseArgText, 200),
                  stringutils::showWSWithClip(aSlice, 200),
                  stringutils::showWSWithClip(bSlice, 200),
                  stringutils::showWSWithClip(newArg, 200));
          } else {
            trace("macro/args",
                  "    lift/paste FAILED argIdx={0} baseArg={1} aSlice={2} "
                  "bSlice={3}",
                  argIdx, stringutils::showWS(baseArgText),
                  stringutils::showWS(aSlice), stringutils::showWS(bSlice));
          }
        }
      }
    }

    // If we've already derived a replacement for this argument index, it must
    // match exactly.
    if (replByArgIdx.count(argIdx) && replByArgIdx[argIdx] != newArg)
      return std::nullopt;

    // Final safety gate for this argument: verify that the candidate replace-
    // ment reproduces all occurrences for argIdx in B, respecting strict/non-
    // strict stringify policy and paste behavior.
    if (!MacroArgReplacementMatchesAllOccurrencesInB(m, argIdx, baseArgText,
                                                     newArg, tokenHunks)) {
      trace(
          "macro/args",
          "    consistency check FAILED for argIdx={0} newArg='{1}' -> expand",
          argIdx, stringutils::showWSWithClip(newArg, 200));
      return std::nullopt;
    }

    trace("macro/args", "    consistency OK for argIdx={0}", argIdx);
    replByArgIdx[argIdx] = std::move(newArg);
  }

  // If nothing required replacement, there is no meaningful args-only patch to
  // emit.
  if (replByArgIdx.empty()) {
    trace("macro/args", "  replByArgIdx empty -> no-op args-only");
    return std::nullopt;
  }

  // Apply replacements to the invocation string. We apply in descending argIdx
  // order so earlier replacements cannot shift the byte ranges of later ones in
  // the same baseInvText.
  std::string finalInv = baseInvText.str();
  auto finalKeys = llvm::to_vector(
      llvm::map_range(replByArgIdx, [](auto &e) { return e.first; }));
  std::sort(finalKeys.begin(), finalKeys.end(), [&](uint32_t a, uint32_t b) {
    return invArgRanges[a].first > invArgRanges[b].first;
  });
  for (uint32_t argIdx : finalKeys) {
    auto r = invArgRanges[argIdx];
    finalInv = stringutils::replaceRange(finalInv, r.first, r.second,
                                         replByArgIdx[argIdx]);
  }

  return MacroPatch{*m.invB, *m.invE, std::move(finalInv)};
}

StringRef RefoldEngine::SliceSource(ArrayRef<size_t> tokOff, StringRef source,
                                    uint64_t startTok, uint64_t endTok) {
  if (tokOff.empty() || source.empty())
    return "";

  const size_t n = tokOff.size();
  const uint64_t maxTokIdx = static_cast<uint64_t>(n) - 1;

  // Clamp token indices to valid array bounds.
  uint64_t loTok = std::clamp(startTok, static_cast<uint64_t>(0), maxTokIdx);
  uint64_t hiTok = std::clamp(endTok, loTok, maxTokIdx);

  size_t lo = tokOff[static_cast<size_t>(loTok)];
  size_t hi = tokOff[static_cast<size_t>(hiTok)];

  // Clamp byte offsets to the actual string length.
  const size_t sourceLen = source.size();
  lo = std::clamp(lo, size_t(0), sourceLen);
  hi = std::clamp(hi, lo, sourceLen);

  return source.substr(lo, hi - lo);
}

std::optional<std::string>
RefoldEngine::UnstringifyLiteralToArgText(StringRef literalTok) {
  StringRef s = literalTok.trim();
  if (s.empty())
    return std::nullopt;

  // Find the opening quote (after any optional prefix)
  size_t q = s.find('\"');
  if (q == StringRef::npos)
    return std::nullopt;

  // Validate the prefix (L, u, U, u8)
  StringRef prefix = s.substr(0, q);
  if (!prefix.empty()) {
    if (prefix != "L" && prefix != "u" && prefix != "U" && prefix != "u8")
      return std::nullopt;
  }

  // Ensure it has a closing quote and is at least ""
  if (s.size() < q + 2 || s.back() != '\"')
    return std::nullopt;

  // Extract content between the quotes
  StringRef body = s.slice(q + 1, s.size() - 1);

  std::string out;
  out.reserve(body.size());

  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '\\' && i + 1 < body.size()) {
      const char n = body[i + 1];
      // Stringification only escapes backslashes and quotes
      if (n == '\\' || n == '\"') {
        out.push_back(n);
        i++;
        continue;
      }
      // Preserve other escape sequences (like \n, \t) as-is
      out.push_back(c);
      out.push_back(n);
      i++;
      continue;
    }
    out.push_back(c);
  }

  // Safety check: if the unstringified text contains a comma, it would be
  // interpreted as multiple arguments in a macro invocation.
  if (out.find(',') != std::string::npos)
    return std::nullopt;

  // Macros cannot have raw newlines in arguments unless escaped/continued
  if (out.find('\n') != std::string::npos ||
      out.find('\r') != std::string::npos)
    return std::nullopt;

  return out;
}

bool RefoldEngine::HunkFullyWithinArgSpans(
    const diffutils::Hunk &h, ArrayRef<RefoldModel::PPArgSpan> argSpans,
    MutableArrayRef<char> touched) const {
  uint64_t a0 = h.aStart;
  uint64_t a1 = h.aEnd;

  auto isCommaTok = [&](uint64_t a) -> bool {
    return a < aToks_.size() &&
           aToks_[static_cast<size_t>(a)].spelling == ",";
  };

  // Insertion: attribute it to the arg span that contains the insertion point.
  // If the insertion lands on a separator comma between two arguments, treat it
  // as belonging to the *right* argument (so prepending into the next argument
  // doesn't spuriously touch the previous one). Otherwise, if the insertion is
  // exactly at an arg-span end (e.g. right before ')'), treat it as belonging to
  // the *left* argument.
  if (a0 == a1) {
    trace("macro/debug", "Checking insertion at A={0}", a0);

    // Prefer strict half-open containment [begin,end).
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      trace("macro/debug", "  Arg {0} span: [{1}, {2})", i, s.begin, s.end);
      if (a0 >= s.begin && a0 < s.end) {
        touched[i] = 1;
        return true;
      }
    }

    // If the insertion is at a comma token, attribute it to the next arg span
    // that begins immediately after the comma (or, failing that, the nearest
    // span to the right).
    if (isCommaTok(a0)) {
      for (size_t i = 0; i < argSpans.size(); ++i) {
        const auto &s = argSpans[i];
        if (s.begin == a0 + 1) {
          touched[i] = 1;
          return true;
        }
      }
      uint64_t bestBegin = UINT64_MAX;
      size_t bestI = static_cast<size_t>(-1);
      for (size_t i = 0; i < argSpans.size(); ++i) {
        const auto &s = argSpans[i];
        if (s.begin > a0 && s.begin < bestBegin) {
          bestBegin = s.begin;
          bestI = i;
        }
      }
      if (bestI != static_cast<size_t>(-1)) {
        touched[bestI] = 1;
        return true;
      }
    }

    // Otherwise, treat a boundary insertion as belonging to the left argument
    // whose span ends at the insertion point.
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (a0 == s.end) {
        touched[i] = 1;
        return true;
      }
    }

    trace("macro/debug", "  FAILED: Point {0} not in any span", a0);
    return false;
  }

  // Replacement/deletion: every covered token must fall inside some arg span.
  // Additionally, allow a separator comma that immediately precedes an arg span
  // (common when deleting the entire variadic tail, which removes the comma
  // after the last fixed formal) to be attributed to that right-hand span.
  bool any = false;
  for (uint64_t a = a0; a < a1; ++a) {
    bool inSome = false;

    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (a >= s.begin && a < s.end) {
        touched[i] = 1;
        inSome = true;
        any = true;
        break;
      }
    }

    if (!inSome && isCommaTok(a)) {
      for (size_t i = 0; i < argSpans.size(); ++i) {
        const auto &s = argSpans[i];
        if (s.begin == a + 1) {
          touched[i] = 1;
          inSome = true;
          any = true;
          break;
        }
      }
    }

    if (!inSome)
      return false;
  }
  return any;
}

std::vector<RefoldEngine::ByteHunk>
RefoldEngine::BuildByteHunksFromRawText() const {
  // We still need a physical array of "elements" for ArrayRef.
  // But now, each element is just 16 bytes (pointer + length)
  // instead of a 32-byte heap-allocating std::string.
  auto ToRefVec = [](StringRef s) {
    std::vector<StringRef> v;
    v.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      // Point to a 1-character substring within the existing aText/bText.
      // This is O(1) and performs NO heap allocation for the character.
      v.push_back(s.substr(i, 1));
    }
    return v;
  };

  std::vector<StringRef> aRefs = ToRefVec(aSource_);
  std::vector<StringRef> bRefs = ToRefVec(bSource_);

  // diff now takes ArrayRef<StringRef>
  auto steps = diffutils::diff(aRefs, bRefs);
  auto hunks = diffutils::coalesce(steps);

  std::vector<ByteHunk> out;
  out.reserve(hunks.size());
  for (const auto &h : hunks) {
    out.emplace_back(h.aStart, h.aEnd, h.bStart, h.bEnd);
  }
  return out;
}

size_t RefoldEngine::MapAByteToBByteLowerBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // 1. Find the first hunk where h.aStart >= aByte
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

  // 2. We need to look at the hunk PRIOR to 'it' to see if aByte falls inside
  // it, or if we are in the gap after it.
  if (it != abByteHunks_->begin()) {
    auto prev = std::prev(it);

    // If the byte is within the previous hunk's range [aStart, aEnd)
    if (searchVal < prev->aEnd) {
      // It's inside an edit/deletion; map to the start of the B-side
      // equivalent.
      return static_cast<size_t>(prev->bStart);
    }
  }

  // 3. Handle the Cumulative Delta.
  int64_t delta = 0;
  for (auto current = abByteHunks_->begin(); current != it; ++current) {
    delta += (static_cast<int64_t>(current->bEnd - current->bStart) -
              static_cast<int64_t>(current->aEnd - current->aStart));
  }

  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldEngine::MapAByteToBByteUpperBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // 1. Binary search finds the first hunk where h.aStart >= aByte
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

  // 3. Accumulate delta from all hunks preceding 'it'
  int64_t delta = 0;
  for (auto current = abByteHunks_->begin(); current != it; ++current) {
    delta += (static_cast<int64_t>(current->bEnd - current->bStart) -
              static_cast<int64_t>(current->aEnd - current->aStart));
  }

  // 4. Handle boundary conditions at the 'it' position
  if (it != abByteHunks_->end()) {
    // If we land exactly on the start of this hunk
    if (searchVal == it->aStart) {
      if (it->aStart == it->aEnd) {
        // Pure insertion at the boundary: Include its shift in the delta.
        delta += static_cast<int64_t>(it->bEnd - it->bStart);
      }
      // Note: We don't snap here because we are at the start of a range.
    } else if (searchVal > it->aStart && searchVal < it->aEnd) {
      // Byte is inside a deletion/replacement hunk: snap to the end.
      return static_cast<size_t>(it->bEnd);
    }
  }

  // 5. Final translation with bounds safety
  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldEngine::BTokIndexFloor(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  const int n = bTokOff_.size() - 1;

  // If the byte is at or before the start of the first token.
  if (bByte <= bTokOff_[0])
    return 0;

  // If the byte is at or after the end of the last token (the sentinel).
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Standard floor binary search.
  while (lo < hi) {
    size_t mid = lo + (hi - lo + 1) / 2;
    size_t off = bTokOff_[mid];
    if (off <= bByte)
      lo = mid;
    else
      hi = mid - 1;
  }

  return lo;
}

size_t RefoldEngine::BTokIndexCeil(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  const int n = bTokOff_.size() - 1;

  // If the byte is at or before the start of the first token.
  if (bByte <= bTokOff_[0])
    return 0;

  // If the byte is at or after the end of the last token (the sentinel).
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Standard ceiling binary search.
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t off = bTokOff_[mid];
    if (off < bByte)
      lo = mid + 1;
    else
      hi = mid;
  }

  return lo;
}

std::pair<size_t, size_t>
RefoldEngine::MapAByteRangeToBTokenEnvelope(size_t aByteBegin,
                                            size_t aByteEnd) const {
  // Sanitize A-byte input range
  if (aByteEnd < aByteBegin)
    aByteEnd = aByteBegin;

  // Convert A-byte span to B-byte span using A→B mapping
  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  // Clamp B-byte bounds to legal range
  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  // Convert B-byte bounds to B-token index span
  size_t bTokBegin = BTokIndexFloor(bByteBegin); // inclusive
  size_t bTokEnd = BTokIndexCeil(bByteEnd);      // exclusive

  // Clamp B-token bounds
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return {bTokBegin, bTokEnd};
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapAToBTokenEnvelopeByPPArgSpan(
    const RefoldModel::PPArgSpan &sp) const {
  // 1. Primary path: Mapping via Preprocessor Byte Spans
  if (sp.ppByteBegin && sp.ppByteEnd) {
    size_t pp0 = static_cast<size_t>(*sp.ppByteBegin);
    size_t pp1 = static_cast<size_t>(*sp.ppByteEnd);
    auto env = MapAByteRangeToBTokenEnvelope(pp0, pp1);
    trace("byte/env",
          "PPArgSpan['{0}' arg={1} Aidx={2} PPbytes=[{3},{4})] -> "
          "Btok=[{5},{6})",
          sp.kind, sp.argIdx, sp.begin, pp0, pp1, env.first, env.second);
    return env;
  }

  // 2. Don't resort to "fallback" if in strict mode.
  if (strict_) {
    fatal("macro/pparg/span",
          "PPArgSpan missing producer ppByte span (kind='{0}' argIdx={1} "
          "A=[{2},{3}) ppByte=[4},{5}]) - cannot map without snapping",
          sp.kind, sp.argIdx, sp.begin, sp.end, sp.ppByteBegin, sp.ppByteEnd);
    return std::nullopt;
  }

  // 3. Fallback: use consumer token offsets.
  const uint64_t maxATok = static_cast<uint64_t>(aToks_.size());
  uint64_t a0Idx = std::clamp(sp.begin, static_cast<uint64_t>(0), maxATok);
  uint64_t a1Idx = std::clamp(sp.end, a0Idx, maxATok);

  size_t a0 = aTokOff_[static_cast<size_t>(a0Idx)];
  size_t a1 = aTokOff_[static_cast<size_t>(a1Idx)];

  return MapAByteRangeToBTokenEnvelope(a0, a1);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelope(uint64_t beginTok,
                                            uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  // If we don't have token data, we can't perform the mapping.
  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  // Standardize the token indices.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);

  // If the range is empty or inverted, return nullopt.
  if (endTok <= beginTok)
    return std::nullopt;

  // Ensure we don't walk off the end of the offset array.
  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size()) {
    // If we are missing the sentinel, we can't safely determine the end of the
    // last token.
    return std::nullopt;
  }

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  // Delegate to the byte-to-token-envelope logic.
  return MapAByteRangeToBTokenEnvelope(aByteBegin, aByteEnd);
}

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldEngine::ParseMacroInvocationArgContentRanges(StringRef invText) {
  // Locate the start of the argument list.
  size_t open = invText.find('(');
  if (open == StringRef::npos)
    return std::nullopt;

  std::vector<std::pair<size_t, size_t>> out;
  size_t n = invText.size();

  uint32_t depth = 0; // Track nested parentheses, brackets, or braces.
  bool inS = false;   // Inside a single-quoted character literal.
  bool inD = false;   // Inside a double-quoted string literal.

  size_t argStart = open + 1;
  for (size_t i = argStart; i < n; i++) {
    char c = invText[i];

    // Handle escaping and termination inside character literals.
    if (inS) {
      if (c == '\\' && i + 1 < n) {
        i++; // Skip escaped characters.
        continue;
      }
      if (c == '\'')
        inS = false;
      continue;
    }

    // Handle escaping and termination inside string literals.
    if (inD) {
      if (c == '\\' && i + 1 < n) {
        i++; // Skip escaped characters.
        continue;
      }
      if (c == '"')
        inD = false;
      continue;
    }

    // Enter literal state if a quote is encountered.
    if (c == '\'') {
      inS = true;
      continue;
    }
    if (c == '"') {
      inD = true;
      continue;
    }

    // Increment depth for nested groups; commas inside these do not
    // separate macro arguments.
    if (c == '(' || c == '[' || c == '{') {
      depth++;
      continue;
    }

    // Handle the closing of a group or the entire argument list.
    if (c == ')') {
      if (depth == 0) {
        // Final argument reached at the closing parenthesis of the call.
        out.push_back(stringutils::trimWsRange(invText, argStart, i));
        return out;
      }
      depth--;
      continue;
    }

    // A comma at depth 0 signifies the end of one macro argument.
    if (c == ',' && depth == 0) {
      out.push_back(stringutils::trimWsRange(invText, argStart, i));
      argStart = i + 1;
    }
  }

  // If the loop finishes without hitting the final ')', the syntax is invalid.
  return std::nullopt;
}

RefoldEngine::IncludePatch
RefoldEngine::BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                         const diffutils::Hunk &h) const {
  // Extra debug: show the raw PP hunk slices.
  DebugIncludePatch("pre", inc, h);

  std::string insertBytes;
  const size_t numOffsets = bTokOff_.size();
  const size_t sourceLen = bSource_.size();

  const size_t uBStart = static_cast<size_t>(h.bStart);
  const size_t uBEnd = static_cast<size_t>(h.bEnd);

  // Validate hunk bounds against B-token offsets.
  if (uBStart < numOffsets && uBEnd < numOffsets && uBEnd >= uBStart) {
    size_t b0 = bTokOff_[uBStart];
    size_t b1 = bTokOff_[uBEnd];

    // Clamp byte offsets to the actual length of bSource_ (defensively handle
    // huge sizes).
    b0 = std::min(b0, sourceLen);
    b1 = std::clamp(b1, b0, sourceLen);
    if (b1 > b0) {
      insertBytes = bSource_.substr(b0, b1 - b0).str();
    }
  } else {
    // Hardening: Log an error or assert if we get a hunk that points
    // outside our known token universe.
    fatal("include/patch",
          "hunk bounds exceed token offset table for inc #{0}: "
          "B[{1},{2}) requested, but bTokOff only has {3} entries",
          inc.id, uBStart, uBEnd, numOffsets);
  }

  IncludePatch patch{&inc,  std::move(insertBytes), h.aStart, h.aEnd, h.bStart,
                     h.bEnd};

  trace("include/patch",
        "built inc #{0} patch A[{1},{2})->B[{3},{4}) len(insertBytes)={5}",
        inc.id, patch.aStart, patch.aEnd, patch.bStart, patch.bEnd,
        patch.insertBytes.size());

  return patch;
}

std::optional<RefoldEngine::MacroPatch>
RefoldEngine::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap) const {
  // The invocation byte span in the owning file must be known.
  const auto invStart = m.invB;
  const auto invEnd = m.invE;
  if (!invStart || *invEnd < *invStart)
    return std::nullopt;

  // Do not downgrade: if we already have a patch for this invocation and it
  // does not look like a callsite invocation anymore (i.e. we already
  // realized/expanded it), keep it.
  auto ownerIt = patchMap.find(m.ownerIncludeId);
  if (ownerIt != patchMap.end()) {
    auto patchIt = ownerIt->second.find(m.id);
    if (patchIt != ownerIt->second.end()) {
      if (!InvocationSpanMatchesCallsitePrefix(patchIt->second.replacement, m))
        return patchIt->second;
    }
  }

  // 1) Prefer args-only patching when safe and fully validated.
  //    Treat normal arg spans, stringify spans, and paste spans as
  //    "argument-like" occurrences.
  SmallVector<RefoldModel::PPArgSpan, 16> argLikeSpans;
  argLikeSpans.append(m.argSpans.begin(), m.argSpans.end());
  argLikeSpans.append(m.stringifySpans.begin(), m.stringifySpans.end());
  argLikeSpans.append(m.pasteSpans.begin(), m.pasteSpans.end());

  SmallVector<char, 16> argTouched(argLikeSpans.size(), 0);
  if (!argLikeSpans.empty() &&
      HunkFullyWithinArgSpans(h, argLikeSpans, argTouched)) {
    // invB/invE are offsets in the invocation file, not in the A-stream source;
    // do not slice aSource here (it can be shorter and/or refer to a different
    // logical file).
    StringRef invSpanText =
        !baseInvText.empty()
            ? baseInvText
            : (m.invText ? StringRef(*m.invText) : StringRef(""));
    if (InvocationSpanMatchesCallsitePrefix(invSpanText, m)) {
      auto argsOnly = BuildMacroInvocationPatchArgsOnly(m, h, baseInvText);
      if (argsOnly)
        return *argsOnly;
    }
  }

  // 2) Whole-cover fallback: replace invocation with the entire expansion cover
  // slice from B. cover.begin/cover.end are PP-token indices in A; map them
  // into a B-token envelope.
  const uint64_t covLoA = m.cover.begin;
  const uint64_t covHiA = m.cover.end;

  if (covLoA >= covHiA)
    return std::nullopt;

  auto bEnv = MapATokRangeAToBTokenEnvelope(covLoA, covHiA);
  if (!bEnv)
    return std::nullopt;

  size_t bTokStart = bEnv->first;
  size_t bTokEnd = bEnv->second;
  if (bTokEnd <= bTokStart)
    return std::nullopt;

  // Tighten the B-side envelope to the exact A-side cover boundary tokens when
  // possible.
  //
  // The A→B alignment/byte-envelope mapping can legitimately drop or shift
  // punctuation tokens (e.g. the leading '(' in an assert() expansion) because
  // they are extremely common and thus low-information in the diff alignment.
  // When that happens, whole-cover replacement can produce syntactically wrong
  // output (missing parens) or double tokens (e.g. ';;').
  //
  // We can correct this deterministically by re-aligning the first/last B
  // tokens to the first/last A tokens of the macro cover, but only when the
  // expected token exists as an immediately-adjacent neighbor in B. This avoids
  // any heuristic scanning.
  if (covLoA < aToks_.size() && bTokStart < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(covLoA)].spelling;
    if (!want.empty()) {
      if (bToks_[bTokStart].spelling != want && bTokStart > 0 &&
          bToks_[bTokStart - 1].spelling == want) {
        bTokStart--;
      }
    }
  }

  if (covHiA > 0 && (covHiA - 1) < aToks_.size() && bTokEnd > 0 &&
      (bTokEnd - 1) < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(covHiA - 1)].spelling;
    if (!want.empty()) {
      if (bToks_[bTokEnd - 1].spelling != want && bTokEnd >= 2 &&
          bToks_[bTokEnd - 2].spelling == want) {
        bTokEnd--;
      }
    }
  }

  // Final check to ensure realignment didn't invert or empty the range.
  if (bTokEnd <= bTokStart)
    return std::nullopt;

  StringRef replacement = SliceBSource(bTokStart, bTokEnd).trim();
  return MacroPatch{*invStart, *invEnd, replacement.str()};
}

bool RefoldEngine::InvocationSpanMatchesCallsitePrefix(
    StringRef invSpanText, const RefoldModel::MacroInvocation &m) const {
  if (invSpanText.empty() || m.name.empty())
    return false;

  const size_t n = invSpanText.size();
  size_t i = 0;

  // Skip leading whitespace.
  while (i < n && stringutils::isWs(invSpanText[i]))
    i++;

  if (i >= n)
    return false;

  // Verify identifier start.
  const char c0 = invSpanText[i];
  if (!stringutils::isIdentStart(c0))
    return false;

  // Find end of identifier.
  size_t j = i + 1;
  while (j < n && stringutils::isIdentPart(invSpanText[j]))
    j++;

  // Check if the identifier matches the macro name.
  StringRef ident = invSpanText.slice(i, j);
  if (ident != m.name)
    return false;

  // Object-like macro: identifier match at span start is sufficient.
  if (m.subkind != "func")
    return true;

  // Function-like macro: must be followed by '(' (allowing whitespace).
  while (j < n && stringutils::isWs(invSpanText[j]))
    j++;

  return (j < n && invSpanText[j] == '(');
}

// =========== Include processing (normalize, materialize, apply) ===========

void RefoldEngine::MaterializeIncludeExpansion(
    uint64_t includeId, const DenseMap<uint64_t, IncludeEdits> &perInclude,
    const DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
        &macroPatchesByOwner,
    const DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
        &children,
    DenseMap<uint64_t, std::string> &includeExpansion) const {
  // Already materialized?
  if (includeExpansion.count(includeId)) {
    debug("include/mat", "SKIP inc#{0} (already materialized)", includeId);
    return;
  }

  const auto *inc = model_.GetIncludeById(includeId);
  if (LLVM_UNLIKELY(!inc)) {
    // This should not be possible, but it's always nice to be defensive.
    fatal("include/mat", "unknown include id {0}", includeId);
  }

  debug("include/mat",
        "ENTER inc#{0} target={1} resolved={2} sitePath={3} site=[{4},{5}) "
        "cover=[{6},{7})",
        inc->id, inc->target, inc->resolvedPath, inc->sitePath, inc->siteB,
        inc->siteE, inc->cover.begin, inc->cover.end);

  const std::string headerPath = resolveHeaderPath(*inc);

  // Start from the raw header text that was preloaded into includeExpansion.
  // If it wasn’t preseeded for some reason, load deterministically by path.
  std::string bytes;
  if (auto it = includeExpansion.find(includeId); it != includeExpansion.end())
    bytes = it->second;
  if (bytes.empty()) {
    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(headerPath));
    if (!bufOrErr) {
      fatal("include/mat", "failed to read header: {0} ({1})", headerPath,
            bufOrErr.getError().message());
    }

    // Treat as raw bytes; copy into std::string
    const MemoryBuffer &mb = **bufOrErr;
    bytes.assign(mb.getBufferStart(), mb.getBufferEnd());
  } else {
    debug("include/mat", "inc#{0} using preseeded bytes len={1}", inc->id,
          bytes.size());
  }

  auto editsIt = perInclude.find(includeId);
  auto macroIt = macroPatchesByOwner.find(includeId);
  auto childIt = children.find(includeId);

  debug(
      "include/mat",
      "inc#{0} initialHeaderLen={1} patches={2} macroPatches={3} children={4}",
      inc->id, bytes.size(),
      (editsIt != perInclude.end()) ? editsIt->second.patches.size() : 0,
      (macroIt != macroPatchesByOwner.end()) ? macroIt->second.size() : 0,
      (childIt != children.end()) ? childIt->second.size() : 0);

  // Collect byte-level edits to apply within this header.
  std::vector<TextEdit> edits;

  // 1) Macro-patch edits owned by this include (invocation byte ranges already
  // in the owner’s file space).
  if (auto it = macroPatchesByOwner.find(includeId);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second) {
      debug("include/mat", "inc#{0} macroPatch inv=[{1},{2}) replLen={3}",
            inc->id, mp.invStart, mp.invEnd, mp.replacement.size());
      ResyncOutcome ro = ApplyResyncOrPend(bytes, mp.invStart, mp.invEnd,
                                           mp.replacement, headerPath);
      edits.push_back(TextEdit{mp.invStart, mp.invEnd, std::move(ro.text),
                               std::move(ro.pending)});
    }
  }

  // 2) A/B include insert/delete/replace patches that belong to this include.
  if (auto it = perInclude.find(includeId); it != perInclude.end()) {
    if (!it->second.patches.empty()) {
      debug("include/mat", "inc#{0} adding {1} include patches as TextEdits",
            inc->id, it->second.patches.size());
      auto moreEdits = ComputeIncludeTextEdits(it->second, bytes);
      for (auto &te : moreEdits) {
        ResyncOutcome ro =
            ApplyResyncOrPend(bytes, te.start, te.end, te.text, headerPath);
        edits.push_back(TextEdit{te.start, te.end, std::move(ro.text),
                                 std::move(ro.pending)});
      }
    } else {
      debug("include/mat", "inc#{0} has no include patches", inc->id);
    }
  } else {
    debug("include/mat", "inc#{0} has no include patches", inc->id);
  }

  auto hasDescendantWork = [&](auto &&self, uint64_t id) -> bool {
    // Direct work at this node?
    bool selfWork = false;
    if (auto it = perInclude.find(id); it != perInclude.end())
      selfWork = !it->second.patches.empty();
    if (!selfWork) {
      if (auto it = macroPatchesByOwner.find(id);
          it != macroPatchesByOwner.end())
        selfWork = !it->second.empty();
    }

    if (selfWork) {
      debug("include/tree", "hasWork inc#{0}: selfWork=YES", id);
      return true;
    }

    // Otherwise, recurse into children
    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        if (self(self, child->id)) {
          debug("include/tree", "hasWork inc#{0}: via child#{1} => YES", id,
                child->id);
          return true;
        }
      }
    }

    debug("include/tree", "hasWork inc#{0}: NO", id);
    return false;
  };

  // 3) Recurse into child includes that have any descendant work, and
  // replace each child directive with the child’s fully materialized text.
  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      const bool todo = hasDescendantWork(hasDescendantWork, child->id);
      debug("include/mat",
            "inc#{0} -> child#{1} target={2} resolved={3} site=[{4},{5}) "
            "expand={6}",
            inc->id, child->id, child->target, child->resolvedPath,
            child->siteB, child->siteE, todo ? "YES" : "NO");
      if (!todo) {
        continue; // leave untouched: keep the original directive as-is
      }

      // Ensure the child is materialized first (depth-first).
      MaterializeIncludeExpansion(child->id, perInclude, macroPatchesByOwner,
                                  children, includeExpansion);

      // The child directive's site is recorded in the includer byte space.
      const auto &childText = includeExpansion[child->id];
      const size_t n = bytes.size();
      const uint64_t siteStart = std::clamp<uint64_t>(child->siteB, 0ULL, n);
      const uint64_t siteEnd = std::clamp<uint64_t>(child->siteE, siteStart, n);

      debug("include/mat",
            "REPLACE in inc#{0}: site=[{1},{2}) len(parent)={3} with child#{4} "
            "len(childText)={5}",
            inc->id, siteStart, siteEnd, bytes.size(), child->id,
            childText.size());

      debug("tu/replace",
            "TU replace site=[{0},{1}) with inc#{2} len={3} (target={4} "
            "resolved={5})",
            siteStart, siteEnd, child->id, childText.size(), child->target,
            child->resolvedPath);

      if (siteStart < siteEnd) {
        std::string childHeaderPath = resolveHeaderPath(*child);
        std::string wrapped = lineDirs_.WrapIncludeExpansion(
            childHeaderPath, headerPath,
            stringutils::lineAtOffset(bytes, child->siteE), childText);
        edits.push_back(
            TextEdit{siteStart, siteEnd, std::move(wrapped), std::nullopt});
      } else {
        // Defensive fallback: if site is somehow unmapped, skip replacing.
        // (This keeps behavior deterministic instead of crashing.)
        debug("include/mat",
              "inc#{0} child#{1} has degenerate site [start={2},end={3}]; "
              "skipping TextEdit",
              inc->id, child->id, siteStart, siteEnd);
      }
    }
  }

  debug("include/mat", "inc#{0} applying {1} header TextEdits", inc->id,
        edits.size());
  std::string applied = ApplyTextEditsWithPendingResync(bytes, edits);
  includeExpansion[includeId] = std::move(applied);
  debug("include/mat", "EXIT inc#{0} resultLen={1}", inc->id,
        includeExpansion[includeId].size());
}

const RefoldModel::HeaderDecl *
RefoldEngine::FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                                     const IncludePatch &p) {
  if (inc.decls.empty())
    return nullptr;

  const uint64_t aLo = p.aStart;
  const uint64_t aHi = p.aEnd;

  const RefoldModel::HeaderDecl *bestCover = nullptr;
  const RefoldModel::HeaderDecl *bestOverlap = nullptr;

  auto getSpanLen = [](const RefoldModel::HeaderDecl *d) -> uint64_t {
    // Standardize: ensure we don't underflow if a span is somehow malformed.
    if (d->span.end <= d->span.begin)
      return 0;
    return d->span.end - d->span.begin;
  };

  for (const auto &d : inc.decls) {
    const uint64_t dLo = d.span.begin;
    const uint64_t dHi = d.span.end;
    const uint64_t curSpanLen = dHi - dLo;

    // Pure insertion: treat as attached at aLo
    if (aLo == aHi) {
      // Special case: insertions at decl end are treated as inclusive
      if (aLo >= dLo && aLo <= dHi) {
        // Tie-breaker: prefer the "tightest" (smallest) declaration that covers
        // this point.
        if (!bestCover || curSpanLen < getSpanLen(bestCover))
          bestCover = &d;
      }
      continue;
    }

    // Non-empty A-interval
    const bool covers = (aLo >= dLo && aHi <= dHi);
    const bool overlaps = (aLo < dHi && aHi > dLo);

    if (covers) {
      // Prioritize "Cover": we want the smallest decl that completely contains
      // the patch.
      if (!bestCover || curSpanLen < getSpanLen(bestCover))
        bestCover = &d;
    } else if (overlaps) {
      // Fallback to "Overlap": if no decl covers it, find the smallest one that
      // touches it.
      if (!bestOverlap || curSpanLen < getSpanLen(bestOverlap))
        bestOverlap = &d;
    }
  }

  return bestCover ? bestCover : bestOverlap;
}

std::vector<RefoldEngine::TextEdit>
RefoldEngine::ComputeIncludeTextEdits(const IncludeEdits &ie,
                                      std::string headerText) const {
  const std::string file = resolveHeaderPath(*ie.include);

  const size_t fileLen = headerText.size();

  debug("include/apply",
        "ENTER computeIncludeTextEdits file={0} len={1} patches={2} "
        "cover=[{3},{4})",
        file, fileLen, ie.patches.size(), ie.include->cover.begin,
        ie.include->cover.end);

  // PP cover for this include inside the header; if not present, these will
  // already have been derived from spans when building the model.
  const uint64_t coverBegin =
      std::max(static_cast<uint64_t>(0), ie.include->cover.begin);
  const uint64_t coverEnd = std::max(coverBegin, ie.include->cover.end);

  // Single list of edits; we will apply them highest-offset-first so indices
  // remain stable as we mutate the StringBuilder.
  std::vector<TextEdit> edits;

  for (size_t idx = 0; idx < ie.patches.size(); ++idx) {
    const IncludePatch &p = ie.patches[idx];
    trace("include/patch", "computeIncludeTextEdits: patch={0}", p);

    const bool isInsert = (p.aStart == p.aEnd) && (p.bStart < p.bEnd);
    const bool isDelete = (p.aStart < p.aEnd) && (p.bStart == p.bEnd);
    const bool isReplace = (p.aStart < p.aEnd) && (p.bStart < p.bEnd);

    debug("include/apply",
          "file={0} patch[{1}] raw A=[{2},{3}) B=[{4},{5}) isInsert={6} "
          "isDelete={7} isReplace={8}",
          file, idx, p.aStart, p.aEnd, p.bStart, p.bEnd, isInsert, isDelete,
          isReplace);

    if (!isInsert && !isDelete && !isReplace) {
      // Ignore empty or malformed patches defensively.
      debug("include/apply",
            "file={0} patch[{1}] ignored (no-op classification)", file, idx);
      continue;
    }

    // Decide which logical header decl "owns" this patch, if any.
    const auto *decl = FindHeaderDeclForPatch(*ie.include, p);

    if (decl) {
      debug("include/apply",
            "file={0} patch[{1}] ownerDecl kind={2} name={3} header=[{4},{5}) "
            "pp-span=[{6},{7})",
            file, idx, decl->kind, decl->name, decl->headerB, decl->headerE,
            decl->span.begin, decl->span.end);
    } else {
      debug("include/apply", "file={0} patch[{1}] ownerDecl=<none>", file, idx);
    }

    // Effective PP coverage in this header we're allowed to touch.
    // For INSERTs we deliberately work at include scope so that inserts that
    // land exactly at a declaration boundary (for example, between
    // `int yyy(...);` and `int zzz(...);`) can anchor to the first token
    // of the following declaration rather than being forced back inside the
    // previous one.  For DELETE / REPLACE we restrict to the owning decl.
    uint64_t ppLo = coverBegin;
    uint64_t ppHi = coverEnd;
    if (!isInsert && decl) {
      ppLo = std::max(ppLo, decl->span.begin);
      ppHi = std::min(ppHi, decl->span.end);
    }
    ppHi = std::max(ppHi, ppLo);

    trace("include/apply",
          "file={0} patch[{1}] effectivePP=[{2},{3}) cover=[{4},{5})", file,
          idx, ppLo, ppHi, coverBegin, coverEnd);

    std::optional<uint64_t> startByte;
    std::optional<uint64_t> endByte;

    const auto &tokmapByPP = model_.GetTokmapByPP();
    if (isInsert) {
      // INSERT: interpret A-position as "before the next token" in this header.
      const uint64_t pos = p.aStart;
      std::optional<uint64_t> anchorPP;

      // 1) Prefer the right neighbor: smallest pp >= pos in [ppLo, ppHi).
      for (uint64_t pp = std::max(pos, ppLo); pp < ppHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          anchorPP = pp;
          break;
        }
      }

      if (anchorPP) {
        // Insert immediately before the right neighbor token.
        startByte = ByteStartForPPInFile(file, *anchorPP,
                                         /* fallbackToEOF */ false, fileLen);
        trace("include/apply",
              "file={0} patch[{1}] INSERT: right-neighbor anchorPP={2} -> "
              "startByte={3}",
              file, idx, anchorPP, startByte);
        if (!startByte) {
          continue;
        }
      } else {
        // 2) No right neighbor; fall back to the last left neighbor.
        if (pos > ppLo && ppHi > ppLo) {
          for (uint64_t pp = std::min(pos - 1, ppHi - 1);; --pp) {
            auto it = tokmapByPP.find(pp);
            if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
              anchorPP = pp;
              break;
            }
            if (pp == ppLo)
              break;
          }
        }

        if (anchorPP) {
          startByte = ByteEndForPPInFile(file, *anchorPP, false, fileLen);
          trace("include/apply",
                "file={0} patch[{1}] INSERT: left-neighbor anchorPP={2} -> "
                "startByte={3}",
                file, idx, anchorPP, startByte);
          if (!startByte) {
            continue;
          }
        } else if (decl) {
          // 3) No PP neighbor at all in this header, but we have an owning
          // decl: anchor at the end of its header span.
          startByte = std::clamp<uint64_t>(decl->headerE, 0ULL, fileLen);
          trace(
              "include/apply",
              "file={0} patch[{1}] INSERT: no neighbors; anchor at declEnd={2}",
              file, idx, startByte);
        } else {
          // 4) Fallback: use child '#include' sites inside this header as
          // synthetic anchors.
          const std::optional<uint64_t> insertByte =
              ComputeChildBoundaryInsertByte(p, file);
          debug("include/apply.", "inserted byte {0}", insertByte);
          if (insertByte && *insertByte <= fileLen) {
            std::string text =
                PadAtBoundaries(headerText, static_cast<size_t>(*insertByte),
                                static_cast<size_t>(*insertByte), p.insertBytes,
                                /* allowLeft */ true, /* allowRight */ true);
            edits.push_back(MakeTextEditWithResyncOrPending(
                headerText, *insertByte, *insertByte, text, file));

            debug("include/apply.",
                  "file={0} patch[{1}] INSERT: anchored via child boundary at "
                  "byte={2}",
                  file, idx, insertByte);
          } else {
            // Preserve old behavior if we still can't place it
            // deterministically.
            debug("include/apply.",
                  "file={0} patch[{1}] INSERT: no neighbors, no decl, no child "
                  "boundary; SKIP",
                  file, idx);
          }
          continue;
        }
      }

      endByte = startByte;
    } else {
      // DELETE / REPLACE: map non-empty A-range to byte range within this
      // header.
      uint64_t aLo = std::max(p.aStart, ppLo);
      uint64_t aHi = std::min(p.aEnd, ppHi);
      if (aHi <= aLo) {
        // Nothing of this patch lies in this header/declaration.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: empty intersection; SKIP",
              file, idx);
        continue;
      }

      std::optional<uint64_t> firstPP, lastPP;
      for (uint64_t pp = aLo; pp < aHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          if (!firstPP)
            firstPP = pp;
          lastPP = pp;
        }
      }

      if (!firstPP || !lastPP) {
        // No tokens from this patch actually map into this header file.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: no mapped PP tokens in "
              "header; SKIP",
              file, idx);
        continue;
      }

      startByte = ByteStartForPPInFile(file, *firstPP,
                                       /* fallbackToEOF */ false, fileLen);
      endByte =
          ByteEndForPPInFile(file, *lastPP, /* fallbackToEOF */ false, fileLen);
      trace("include/apply",
            "file={0} patch[{1}] DELETE/REPLACE: firstPP={2} lastPP={3} -> "
            "bytes=[{4},{5})",
            file, idx, firstPP, lastPP, startByte, endByte);
      if (!startByte || !endByte) {
        continue;
      }
    }

    // Clamp to the declaration’s header_span, if any, so we never cross decl
    // boundaries for DELETE/REPLACE.  For INSERTs we intentionally allow the
    // anchor to sit on the boundary between two declarations so that a new
    // declaration can be injected cleanly between them.
    if (!isInsert && decl) {
      startByte = std::clamp(*startByte, decl->headerB, decl->headerE);
      endByte = std::clamp(*endByte, decl->headerB, decl->headerE);
    }

    // Sanity clamp to file bounds.
    if (!startByte)
      continue;

    const uint64_t fLen = static_cast<uint64_t>(fileLen);
    startByte = std::clamp(*startByte, uint64_t(0), fLen);
    endByte = std::clamp(*endByte, *startByte, fLen);

    std::string replacement = isDelete ? "" : p.insertBytes;

    if (inTraceMode()) {
      StringRef opKind =
          isInsert ? "INSERT" : (isDelete ? "DELETE" : "REPLACE");

      // DEBUG: log the exact slice and replacement we are about to apply.
      const std::string originalSlice =
          headerText.substr(*startByte, *endByte - *startByte);
      std::string origDbg = originalSlice;
      std::string replDbg = replacement;
      constexpr int MAX_DBG = 120;
      if (origDbg.size() > MAX_DBG) {
        origDbg = origDbg.substr(0, MAX_DBG) + "…";
      }
      if (replDbg.size() > MAX_DBG) {
        replDbg = replDbg.substr(0, MAX_DBG) + "…";
      }

      std::string declInfo = "<none>";
      if (decl) {
        declInfo =
            formatv("kind={0} name={1} header=[{2},{3}) pp-span=[{4},{5})",
                    decl->kind, decl->name, decl->headerB, decl->headerE,
                    decl->span.begin, decl->span.end)
                .str();
      }

      trace("include/apply",
            "file={0} patch[{1}] kind={2} A=[{3},{4}) B=[{5},{6}) pp=[{7},{8}) "
            "bytes=[{9},{10}) decl={11} orig='{12}' repl='{13}'",
            file, idx, opKind, p.aStart, p.aEnd, p.bStart, p.bEnd, ppLo, ppHi,
            startByte, endByte, declInfo, stringutils::showWS(origDbg),
            stringutils::showWS(replDbg));
    }

    edits.push_back(MakeTextEditWithResyncOrPending(
        headerText, *startByte, *endByte, replacement, file));
  }

  // Apply all edits inside this header, highest offset first so earlier edits
  // do not disturb the coordinates of later ones.
  sort(edits, [](const TextEdit &lhs, const TextEdit &rhs) {
    if (lhs.start != rhs.start)
      return lhs.start > rhs.start;
    return lhs.end > rhs.end;
  });

  debug("include/apply", "file={0} computed {1} header TextEdits", file,
        edits.size());

  for (const auto &e : edits) {
    trace("include/apply",
          "file={0} header TextEdit bytes=[{1},{2}) replLen={3}", file, e.start,
          e.end, e.text.size());

    if (e.end < e.start || e.end > static_cast<uint64_t>(headerText.size())) {
      fatal("include/apply", "TextEdit out of bounds: bytes=[{0},{1}) size={2}",
            e.start, e.end, headerText.size());
    }
  }

  return edits;
}

std::optional<uint64_t>
RefoldEngine::ComputeChildBoundaryInsertByte(const IncludePatch &p,
                                             StringRef file) const {
  // Which include are we editing?
  const RefoldModel::IncludeItem *owner = p.include;
  if (!owner) {
    return std::nullopt;
  }

  // We only care about children whose sitePath is this header file.
  const RefoldModel::IncludeItem *left =
      nullptr; // last child whose coverEnd <= pos
  const RefoldModel::IncludeItem *right =
      nullptr; // first child whose coverBegin >= pos

  uint64_t pos = p.aStart; // PP position of the INSERT gap

  for (const auto &child : model_.GetIncludes()) {
    // Only check direct children of the owner
    if (!child.parent || *child.parent != owner->id) {
      continue;
    }

    // Paths must match the file currently being processed
    if (!PathsEqual(child.sitePath, file)) {
      continue;
    }

    uint64_t cb = child.cover.begin;
    uint64_t ce = child.cover.end;

    // If the INSERT PP-index is *strictly inside* a child's cover, this
    // fallback is the wrong mechanism (that should have been a child-owned
    // patch). Gaps on the boundaries (pos == cb or pos == ce) are valid
    // "between-children" positions and must *not* trigger this guard.
    if (cb < pos && pos < ce) {
      return std::nullopt;
    }

    if (ce <= pos) {
      // Best "left" child is the one with the greatest coverEnd <= pos.
      if (!left || ce > left->cover.end) {
        left = &child;
      }
    } else if (cb >= pos) {
      // Best "right" child is the one with the smallest coverBegin >= pos.
      if (!right || cb < right->cover.begin) {
        right = &child;
      }
    }
  }

  if (right) {
    // Insert *before* the right '#include'.
    return right->siteB;
  }
  if (left) {
    // Insert *after* the left '#include'.
    return left->siteE;
  }

  return std::nullopt;
}

RefoldEngine::ResyncOutcome
RefoldEngine::ApplyResyncOrPend(StringRef originalFileText, uint64_t start,
                                uint64_t end, StringRef replacement,
                                StringRef fileSpellingForDirective) const {
  size_t origNl = stringutils::countNewlines(originalFileText, start, end);
  size_t replNl =
      stringutils::countNewlines(replacement, 0, replacement.size());
  if (origNl == replNl)
    return ResyncOutcome(replacement.str(), std::nullopt);

  // Drift detected: decide whether to inject a local `#line` or defer.
  size_t resumeLine = stringutils::lineAtOffset(originalFileText, end);
  trace("linedir/resync",
        "drift: span=[{0},{1}) origNl={2} replNl={3} resumeLine={4} file={5} "
        "replTail={6}",
        start, end, origNl, replNl, resumeLine, fileSpellingForDirective,
        stringutils::showWSWithClip(replacement, 100));

  // Attempt local injection first.
  std::string injected = lineDirs_.MaybeAppendResyncAfterReplacement(
      originalFileText, start, end, replacement, fileSpellingForDirective);

  // If the returned string changed, injection succeeded.
  if (injected != replacement) {
    trace("linedir/resync", "local inject succeeded: resumeLine={0} file={1}",
          resumeLine, fileSpellingForDirective);
    return ResyncOutcome(std::move(injected), std::nullopt);
  }

  trace("linedir/resync",
        "local inject failed -> PENDING: resumeLine={0} file={1}", resumeLine,
        fileSpellingForDirective);

  return ResyncOutcome{replacement.str(),
                       PendingResync{fileSpellingForDirective}};
}

std::string
RefoldEngine::ApplyTextEditsWithPendingResync(StringRef originalFileText,
                                              ArrayRef<TextEdit> edits) const {
  if (edits.empty())
    return originalFileText.str();

  DenseMap<std::pair<uint64_t, uint64_t>, const TextEdit *> bySpan;
  for (const auto &e : edits) {
    bySpan[{e.start, e.end}] = &e;
  }

  // Use SmallVector for the normalized list to stay on the stack if possible.
  auto values = llvm::make_second_range(bySpan);
  SmallVector<const TextEdit *, 32> norm(values.begin(), values.end());

  sort(norm, [](const TextEdit *a, const TextEdit *b) {
    if (a->start != b->start)
      return a->start < b->start;
    return a->end < b->end;
  });

  // Use SmallString for the output buffer to optimize small file edits.
  SmallString<0> out;
  out.reserve(originalFileText.size() + 128);
  std::optional<PendingResync> pending = std::nullopt;

  uint64_t cursor = 0;
  size_t n = originalFileText.size();

  for (const auto *e : norm) {
    if (e->end < e->start || e->end > n) {
      fatal("edits/apply", "bad edit bounds [{0},{1}) fileLen={2}", e->start,
            e->end, n);
    }
    if (e->start < cursor) {
      fatal("edits/apply", "overlapping edits: cursor={0} nextStart={1}",
            cursor, e->start);
    }

    // Pass the SmallString to the appender.
    pending = AppendOriginalSliceWithPending(out, originalFileText, cursor,
                                             e->start, std::move(pending));

    out.append(e->text);

    if (e->pending) {
      pending = e->pending;
    }

    cursor = e->end;
  }

  pending = AppendOriginalSliceWithPending(out, originalFileText, cursor, n,
                                           std::move(pending));

  return std::string(out.str());
}

std::optional<RefoldEngine::PendingResync>
RefoldEngine::AppendOriginalSliceWithPending(
    SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
    uint64_t to, std::optional<RefoldEngine::PendingResync> pending) const {
  if (!pending || !lineDirs_.Enabled()) {
    auto slice = original.slice(from, to);
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  uint64_t i = from;

  // Only flush immediately if BOTH:
  // (1) output is at BOL, and
  // (2) the next original slice begins at BOL in the original file
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      stringutils::isBOL(original, static_cast<size_t>(from))) {
    size_t line = stringutils::lineAtOffset(original, from);
    std::string directive =
        lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

    // Create a view of the current buffer for the check
    StringRef currentOut(out.data(), out.size());

    if (LineDirectiveInserter::ShouldEmitLineDirective(
            currentOut, pending->fileSpellingForDir, line, directive)) {
      trace("line/pending",
            "flush@slice-begin file={0} line={1} (pending) outTail={2}",
            pending->fileSpellingForDir, line,
            stringutils::dbgOutTail(currentOut));
      out.append(directive.begin(), directive.end());
    } else {
      trace("line/pending",
            "SKIP flush@slice-begin (no-op) file={0} line={1} outTail={2}",
            pending->fileSpellingForDir, line,
            stringutils::dbgOutTail(currentOut));
    }

    auto slice =
        original.slice(static_cast<size_t>(from), static_cast<size_t>(to));
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  // Otherwise, scan forward for the first safe newline boundary (not
  // line-spliced).
  while (i < to) {
    size_t nl = original.find('\n', i);
    if (nl == llvm::StringRef::npos || nl >= static_cast<size_t>(to))
      break;

    auto slice = original.slice(static_cast<size_t>(i), nl + 1);
    out.append(slice.begin(), slice.end());

    i = nl + 1;

    if (!stringutils::isLineSplice(original, nl)) {
      size_t line = stringutils::lineAtOffset(original, static_cast<size_t>(i));
      std::string directive =
          lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

      // Update our view of the output after appending the newline
      StringRef currentOut(out.data(), out.size());
      if (LineDirectiveInserter::ShouldEmitLineDirective(
              currentOut, pending->fileSpellingForDir, line, directive)) {
        trace("line/pending",
              "flush@safe-nl file={0} line={1} atOrig={2} outTail={3}",
              pending->fileSpellingForDir, line, i,
              stringutils::dbgOutTail(currentOut));
        out.append(directive.begin(), directive.end());
      } else {
        trace("line/pending",
              "SKIP flush@safe-nl (no-op) file={0} line={1} atOrig={2} "
              "outTail={3}",
              pending->fileSpellingForDir, line, i,
              stringutils::dbgOutTail(currentOut));
      }

      pending = std::nullopt;
      break;
    }
  }

  // Clean up remaining bytes
  if (i < to) {
    auto remaining =
        original.slice(static_cast<size_t>(i), static_cast<size_t>(to));
    out.append(remaining.begin(), remaining.end());
  }
  return pending;
}

// ==================== Low-level file & mapping utilities =====================

bool RefoldEngine::PathsEqual(StringRef a, StringRef b) {
  if (a.empty() || b.empty())
    return a == b;

  std::error_code ecA, ecB;
  const auto ca =
      std::filesystem::weakly_canonical(std::filesystem::path(a.str()), ecA);
  const auto cb =
      std::filesystem::weakly_canonical(std::filesystem::path(b.str()), ecB);

  if (ecA) {
    fatal("path/canon", "failed to canonicalize '{0}': {1}", a, ecA.message());
  }
  if (ecB) {
    fatal("path/canon", "failed to canonicalize '{0}': {1}", b, ecB.message());
  }

  return ca == cb;
}

// ====================== Diagnostics & Debug Utilities =======================

void RefoldEngine::DebugIncludePatch(StringRef tag,
                                     const RefoldModel::IncludeItem &inc,
                                     const diffutils::Hunk &h) const {
  auto getByteOff = [](uint64_t tokenIdx,
                       ArrayRef<size_t> offsets) -> std::optional<uint64_t> {
    if (tokenIdx < offsets.size()) {
      return static_cast<uint64_t>(offsets[static_cast<size_t>(tokenIdx)]);
    }
    return std::nullopt;
  };

  const std::optional<uint64_t> a0 = getByteOff(h.aStart, aTokOff_);
  const std::optional<uint64_t> a1 = getByteOff(h.aEnd, aTokOff_);
  const std::optional<uint64_t> b0 = getByteOff(h.bStart, bTokOff_);
  const std::optional<uint64_t> b1 = getByteOff(h.bEnd, bTokOff_);

  StringRef aSlice = "";
  if (a0 && a1 && *a1 >= *a0 && *a1 <= aSource_.size()) {
    aSlice = aSource_.substr(static_cast<size_t>(*a0),
                             static_cast<size_t>(*a1 - *a0));
  }
  StringRef bSlice = "";
  if (b0 && b1 && *b1 >= *b0 && *b1 <= bSource_.size()) {
    bSlice = bSource_.substr(static_cast<size_t>(*b0),
                             static_cast<size_t>(*b1 - *b0));
  }

  trace("include/patch",
        "{0} inc #{1} hunk A[{2},{3})->B[{4},{5}) Abytes=[{6},{7}) "
        "Bbytes=[{8},{9}) Aslice='{10}' Bslice='{11}'",
        tag, inc.id, h.aStart, h.aEnd, h.bStart, h.bEnd, a0, a1, b0, b1,
        stringutils::showWSWithClip(aSlice, 120),
        stringutils::showWSWithClip(bSlice, 120));
}

std::string RefoldEngine::PPArgSpanToString(const RefoldModel::PPArgSpan &sp,
                                            bool isStringifyOcc) {
  std::string storage;
  llvm::raw_string_ostream os(storage);

  os << formatv("{kind='{0}', A=[{1},{2}), argIdx={3}", sp.kind, sp.begin,
                sp.end, sp.argIdx);

  if (isStringifyOcc) {
    os << ", occ=STRINGIFY";
  }

  if (sp.kind == PPArgSpanKind::Paste) {
    os << formatv(", byte=[{0},{1})", sp.byteBegin, sp.byteEnd);
  }

  os << '}';
  return os.str();
}

std::string
RefoldEngine::PPArgSpanListToString(ArrayRef<RefoldModel::PPArgSpan> spans,
                                    ArrayRef<char> isStringify) {
  std::string storage;
  llvm::raw_string_ostream os(storage);

  os << '[';
  for (size_t i = 0; i < spans.size(); ++i) {
    if (i > 0)
      os << ", ";

    bool isStr = (i < isStringify.size() && isStringify[i]);
    os << PPArgSpanToString(spans[i], isStr);
  }
  os << ']';

  return os.str();
}

} // namespace refold
} // namespace clang
