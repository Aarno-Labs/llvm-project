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
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <limits>

using namespace llvm;

namespace clang {
namespace refold {

namespace {
constexpr int kNoOwner = -1;

inline std::string resolveHeaderPath(const RefoldModel::IncludeItem &inc) {
  return (inc.resolvedPath && !inc.resolvedPath->empty())
             ? *inc.resolvedPath
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
  std::string tuBytes;
  {
    const auto fullTuPath = lineDirs_.ToAbsolutePath(tuPath);
    auto bufOrErr = MemoryBuffer::getFile(fullTuPath);
    if (!bufOrErr) {
      // Fatal and stop: unreachable past this point.
      fatal("src/load", "failed to read C source: {0} ({1})", fullTuPath,
            bufOrErr.getError().message());
    }

    // Copy the file bytes into a std::string (UTF-8 is treated as raw bytes
    // here).
    tuBytes.assign(bufOrErr.get()->getBufferStart(),
                   bufOrErr.get()->getBufferEnd());
  }

  constexpr size_t MAX_COLS = 80;
  SmallString<MAX_COLS> sepBuf;
  sepBuf.assign(MAX_COLS, '-');
  StringRef sep = sepBuf;

  // 1) Generate token sequences and A->B anchor map.
  auto aSeq = MapLexemes(aToks_, aTokOff_);
  trace("lcs/aSeq", "aSeq:");
  trace("lcs/aSeq", "=====");
  logFormattedArray<std::string>(aSeq, /* k */ MAX_COLS,
                                 /* sameWidth */ false,
                                 [](StringRef msg) { trace("lcs/aSeq", msg); });
  trace("lcs/aSeq", sep);

  auto bSeq = MapLexemes(bToks_, bTokOff_);
  trace("lcs/bSeq", "bSeq:");
  trace("lcs/bSeq", "=====");
  logFormattedArray<std::string>(bSeq, /* k */ MAX_COLS,
                                 /* sameWidth */ false,
                                 [](StringRef msg) { trace("lcs/bSeq", msg); });
  trace("lcs/bSeq", sep);

  // 1b) Compute per-gap ownership depth for A's PP tokens.
  std::vector<unsigned> ownerDepthGap =
      ComputeOwnerDepthGapsForPP(aTokOff_.size());
  trace("lcs/ownerGap", "ownerDepthGap:");
  trace("lcs/ownerGap", "==============");
  logFormattedArray<unsigned>(
      ownerDepthGap, /* k */ MAX_COLS, /* sameWidth */ true,
      [](StringRef msg) { trace("lcs/ownerGap", msg); });
  trace("lcs/bSeq", sep);

  // 2) LCS over tokens (A → B) with owner-aware cost model.
  auto a2b = diffutils::lcsMapAB(aSeq, bSeq, ownerDepthGap);
  trace("lcs/a2b", "a2b:");
  trace("lcs/a2b", "====");
  logFormattedArray<int>(a2b, /* k */ MAX_COLS, /* sameWidth */ true,
                         [](StringRef msg) { trace("lcs/a2b", msg); });
  trace("lcs/a2b", sep);

#if 0
  auto writeMap = [](StringRef filename, const std::vector<int> &a2b) {
    std::error_code ec;
    raw_fd_ostream os(filename.str(), ec, llvm::sys::fs::OF_Text);
    if (ec) {
      fatal("a2b/write", "open file '{0}' failed: {1}", filename, ec.message());
    }

    for (int v : a2b)
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
  int last = -1;
  for (size_t i = 0; i < a2b.size(); ++i) {
    int j = a2b[i];
    if (j < 0)
      continue;
    if (j < last) {
      fatal("lcs/map", "non-monotone map at A[{0}]={1} after {2}", i, j, last);
    }
    last = j;
  }

  debug("lcs", "A={0} toks, B={1} toks", aSeq.size(), bSeq.size());
  int mapped = 0;
  for (int v : a2b) {
    if (v >= 0)
      mapped++;
  }
  debug("lcs", "mapped A->B = {0} ({1:F1}%)", mapped,
        100.0 * mapped / std::max<size_t>(1U, aSeq.size()));

  info("plan", "TU={0} includes={1} macroInvocations={2} tokmap={3}", tuPath,
       model_.GetIncludes().size(), model_.GetMacroInvocations().size(),
       model_.GetTokmapByPP().size());

  // 3) Diff hunks (changed A-token intervals -> B-token intervals).
  auto hunks = diffutils::hunksFromMap(a2b, static_cast<int>(aSeq.size()),
                                       static_cast<int>(bSeq.size()));

  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    StringRef bfrag;
    if (h.bStart < h.bEnd) {
      const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];
      const size_t b1 = bTokOff_[static_cast<size_t>(h.bEnd)];
      // Defensive clamping (should already be valid since offsets have
      // sentinel):
      const size_t lo = std::max<size_t>(0U, b0);
      const size_t hi = std::max<size_t>(lo, b1);
      bfrag = bSource_.substr(lo, hi - lo);
    }

    std::string shown = stringutils::showWSWithClip(bfrag.str(), 160);
    debug("hunks", "#{0} {1:verbose} B='{2}'", i, h, shown);
  }

  // 4) Classify hunks and collect per-target edits.
  std::vector<TextEdit> tuEdits;
  DenseMap<int, IncludeEdits> perInclude; // includeId -> edits
  DenseMap<int, std::vector<MacroPatch>> macroPatchesByOwner;

  // Merge macro patches by macro-invocation id so multiple arg hunks compose
  // correctly.
  DenseMap<int, DenseMap<int, MacroPatch>> macroPatchByOwnerByMacroId;

  // Iterate over all hunks:
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // Shape info (pure insert/delete/replace) – LOG ONLY
    bool isIns = (h.aStart == h.aEnd) && (h.bStart < h.bEnd);
    bool isDel = (h.aStart < h.aEnd) && (h.bStart == h.bEnd);
    bool isRep = (h.aStart < h.aEnd) && (h.bStart < h.bEnd);
    debug("classify", "#{0} shape: isIns={1} isDel={2} isRep={3} {4}", i, isIns,
          isDel, isRep, h);

    // a) Segment-aware owner classification: this decides TU vs include vs “no segment”.
    debug("classify", "#{0} -> calling classifyOwnerWithSegments {1}", i, h);
    Owner owner = ClassifyOwnerWithSegments(tuPath, h);
    debug(
        "classify",
        "#{0} ownerFromSegments kind={1} includeId={2} condArmId={3} {4}", i,
        owner.kind, owner.includeId, owner.condArmId, h);

    // b) Macro call-site still has priority over TU/include
    if (auto *m = SmallestCoveringPatchableMacro(h.aStart, h.aEnd)) {
      if (m->GetInvB() != -1 && m->GetInvE() != -1) {
        debug("classify",
              "#{0} -> MACRO invText={1} owner={2} invFile={3} {4})", i,
              m->invText, m->ownerIncludeId, m->invFile, h);
        const auto macroOwner = m->ownerIncludeId;
        auto &byMacroId =
            macroPatchByOwnerByMacroId[macroOwner ? *macroOwner : kNoOwner];
        auto existingIt = byMacroId.find(m->id);

        // If found, use the existing replacement; otherwise, use the original
        // text.
        std::string currentInvText = (existingIt != byMacroId.end())
                                         ? existingIt->second.replacement
                                         : (m->invText ? *m->invText : "");
        auto updated = BuildMacroInvocationPatchWholeCover(
            *m, h, a2b, currentInvText, macroPatchByOwnerByMacroId);
        byMacroId[m->id] = std::move(updated);
        continue;
      }
    }

    // c) Special case: pure insertions exactly at the boundary between sibling
    // includes that share a common parent. In this case, per policy, the
    // insertion should be attached to the *parent* include so that the
    // refolded C places it between `#include` lines, not inside any child.
    if (isIns) {
      const RefoldModel::IncludeItem *parentBoundaryInc =
          BoundaryParentIncludeForPureInsertion(h, tuPath);
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
        int firstCond = model_.FirstConditionalArmStartA(*owner.includeId);
        if (h.aStart <= firstCond) {
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

    if (owner.kind == OwnerKind::TU) {
      if (isIns) {
        // For pure insertions, trust the segment classification.
        // We already anchored the probe in TU byte space via tuByteSpan.
        mapsToTU = true;
      }

      if (!mapsToTU) {
        // Only demote for non-empty A-side hunks (real edits/deletes),
        // where we *must* make sure we’re not accidentally editing header
        // text and calling it “TU”.
        owner = Owner::Unknown();
      }
    }

    if (mapsToTU && owner.kind == OwnerKind::Include && owner.includeId) {
      debug("classify",
            "#{0} DIAGNOSTIC: tokmap says TU but segments say INCLUDE(id={1}); "
            "will still treat as TU (tokmap wins).",
            i, owner.includeId);
    }

    if (mapsToTU) {
      auto span = TUByteSpan(h.aStart, h.aEnd, tuPath); // [b,e)
      debug("classify", "#{0} TU-byteSpan=[{1},{2}) for A[{3},{4})", i,
            span.first, span.second, h.aStart, h.aEnd);
      std::string repl;
      if (span.first >= 0 && h.bStart < h.bEnd) {
        size_t b0 = bTokOff_[h.bStart], b1 = bTokOff_[h.bEnd];
        repl.assign(bSource_.data() + b0, bSource_.data() + b1);
      }

      if (span.first >= 0 && span.second >= 0) {
        // Is this span replacing a TU "gap" (bytes that are all whitespace)?
        std::string original;
        if (span.second > span.first) {
          original.assign(tuBytes.data() + span.first,
                          tuBytes.data() + span.second);
        } else if (span.second < span.first) {
          fatal("tu/span", "invalid TU byte span: [{0},{1})", span.first,
                span.second);
        }
        bool replacingGap =
            !original.empty() && stringutils::isAsciiWhitespace(original);

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
            PadAtBoundaries(tuBytes, static_cast<size_t>(span.first),
                            static_cast<size_t>(span.second), std::move(repl),
                            /*allowLeft*/ !replacingGap,
                            /*allowRight*/ true);

        debug("classify",
              "#{0} -> TU  bytes=[{1},{2}) rawRepl='{3}' paddedRepl='{4}'", i,
              span.first, span.second, stringutils::showWSWithClip(repl, 160),
              stringutils::showWSWithClip(padded, 160));

        ResyncOutcome ro =
            ApplyResyncOrPend(tuBytes, span.first, span.second, padded, tuPath);
        tuEdits.push_back(TextEdit{span.first, span.second, std::move(ro.text),
                                   std::move(ro.pending)});
        continue;
      } else {
        debug("classify",
              "#{0} TU mapping had span.first < 0; TU edit skipped (behavior "
              "unchanged).",
              i);
      }
    }

    // f) Fallback: if segments did not classify and it isn't TU/macro, we try
    // the old smallestCoveringInclude as a last resort for backwards
    // compatibility.
    debug("classify",
          "#{0} entering fallback smallestCoveringInclude; owner.kind={1} "
          "includeId={2}",
          i, owner.kind, owner.includeId);
    if (auto *inc = SmallestCoveringInclude(h.aStart, h.aEnd)) {
      const std::string headerPath = resolveHeaderPath(*inc);
      debug("classify",
            "#{0} -> INCLUDE id={1} path={2}  {3}  (fallback "
            "smallestCoveringInclude)",
            i, inc->id, headerPath, h);
      auto [it, _] = perInclude.try_emplace(inc->id, inc);
      it->second.Add(BuildIncludeInsertionPatch(*inc, h));
      continue;
    }

    fatal(
        "hunks",
        "no covering item (macro/include/TU) for changed A-interval [{0},{1})",
        h.aStart, h.aEnd);
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
  DenseMap<int, std::vector<const RefoldModel::IncludeItem *>> children;
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
  DenseMap<int, std::string> includeExpansion;

  // Build the set of include-ids that must be realized.
  DenseSet<int> seeds;

  // (a) Direct include edits.
  auto perIncludeKeys = make_first_range(perInclude);
  seeds.insert(perIncludeKeys.begin(), perIncludeKeys.end());

  // (b) Macro-owned work INSIDE headers (ownerIncludeId != null).
  for (auto &kv : macroPatchesByOwner) {
    if (kv.first != kNoOwner)
      seeds.insert(kv.first);
  }

  // (c) Pull in all ancestors up to the TU.
  for (int id : std::vector<int>(seeds.begin(), seeds.end())) {
    const auto *cur = model_.GetIncludeById(id);
    while (cur && cur->parent) {
      seeds.insert(*cur->parent);
      cur = model_.GetIncludeById(*cur->parent);
    }
  }

  std::vector<int> seedsVec(seeds.begin(), seeds.end());
  trace("include/mat", "seeds:");
  trace("include/mat", "======");
  logFormattedArray<int>(seedsVec, /* k */ MAX_COLS,
                         /* sameWidth */ false,
                         [](StringRef msg) { trace("include/mat", msg); });
  trace("include/mat", sep);

  // (d) Realize each include once (memoization lives inside
  // MaterializeIncludeExpansion).
  for (int incId : seeds) {
    debug("include/mat", "materialize seed include #{0}", incId);
    MaterializeIncludeExpansion(incId, perInclude, macroPatchesByOwner,
                                children, includeExpansion);
  }

  // 6a) TU macro patches (ownerIncludeId == kNoOwner) and include expansions
  // at TU sites.
  if (auto it = macroPatchesByOwner.find(kNoOwner);
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
      // If the spans are exactly equal, require that the replacement be
      // identical as well (otherwise we'd be choosing one arbitrarily,
      // violating determinism).
      /*
       * TODO: Should we keep this, or should this be possible (i.e. should we
       * warn and continue instead)??
       */
      if (mp.invStart < 0 || mp.invEnd < 0) {
        fatal("macro/tu",
              "TU macro patch has either a negative invocation start or end "
              "(invStart={0} invEnd={1})",
              mp.invStart, mp.invEnd);
      }

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
          fatal("macro/tu", "overlapping TU macro patches: mp=[{0},{1}) acc=[{2},{3})",
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

std::vector<std::string> RefoldEngine::MapLexemes(ArrayRef<PPTok> toks,
                                                  ArrayRef<size_t> offs) {
  std::vector<std::string> out;
  out.reserve(toks.size());
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const auto &s = toks[i].spelling;
    if (stringutils::isAsciiWhitespace(s)) {
      // position-tied, cannot anchor elsewhere
      out.emplace_back("WS@" + std::to_string(offs[i]));
    } else {
      out.emplace_back(s);
    }
  }
  return out;
}

std::vector<unsigned>
RefoldEngine::ComputeOwnerDepthGapsForPP(size_t numOfAOffs) {
  // aTokOff.size() == (#tokens) + 1 (sentinel). LCS expects N == #tokens,
  // and ownerDepthGap.size() == N + 1.
  const size_t N = numOfAOffs - 1;
  std::vector<unsigned> ownerDepthGap(N + 1, 0);

  for (size_t k = 0; k <= N; ++k) {
    // --------------------------- Include depth ----------------------------
    std::optional<int> leftInc;
    std::optional<int> rightInc;

    if (k > 0) {
      leftInc = model_.InnermostIncludeAtPP(k - 1);
    }
    if (k < N) {
      rightInc = model_.InnermostIncludeAtPP(k);
    }

    std::optional<int> lca =
        model_.LeastCommonAncestorInclude(leftInc, rightInc);
    unsigned incDepth = model_.GetIncludeDepth(lca);

    // ------------------------- Conditional depth --------------------------
    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (k > 0) {
      leftArmRef = model_.FindArmRefAtPP(k - 1);
    }
    if (k < N) {
      rightArmRef = model_.FindArmRefAtPP(k);
    }

    unsigned leftCondDepth =
        leftArmRef ? model_.GetCondArmDepth(leftArmRef->arm->id) : 0;
    unsigned rightCondDepth =
        rightArmRef ? model_.GetCondArmDepth(rightArmRef->arm->id) : 0;
    unsigned condDepth = std::min(leftCondDepth, rightCondDepth);

    ownerDepthGap[k] = incDepth + condDepth;
  }

  return ownerDepthGap;
}

// ============================= Boundary helpers ==============================

bool RefoldEngine::BoundaryGlues(char left, char right) {
  const bool leftId = stringutils::isIdentChar(left);
  const bool rightId = stringutils::isIdentChar(right);
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
  const char leftC = (start > 0 && start <= base.size()) ? base[start - 1] : '\0';
  const char rightC = (end < base.size()) ? base[end] : '\0';

  // Check for existing whitespace at the edges of the provided text
  const bool hasLeadingWS = (*f > 0);
  const bool hasTrailingWS = (*l + 1 < text.size());

  // Determine if padding is needed BEFORE modifying the string to avoid index drift
  bool addLeftSpace = allowLeft && !hasLeadingWS && BoundaryGlues(leftC, text[*f]);
  bool addRightSpace = allowRight && !hasTrailingWS && BoundaryGlues(text[*l], rightC);

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
  int a0 = h.aStart;
  int a1 = h.aEnd;

  debug("segments",
        "ENTER classifyOwnerWithSegments tuPath={0} A[{1},{2}) (isEmpty={3})",
        tuPath, a0, a1, a0 == a1);

  // First, get the TU byte span for this hunk. Even when the hunk ultimately
  // belongs to a header, we still anchor via the TU span because segments for
  // includes and conditional arms in that header are projected into the TU
  // through slots.
  auto span = TUByteSpan(a0, a1, tuPath); // [b, e)

  // If there is no truthful TU anchor (no TU tokens in the range, and the
  // insertion cannot be safely anchored in TU), classify purely in PP space.
  if (span.first < 0 || span.second < 0) {
    const bool isInsert = (a0 == a1);
    const auto &tokmapByPP = model_.GetTokmapByPP();
    const size_t n = tokmapByPP.size();

    std::optional<int> leftInc;
    std::optional<int> rightInc;

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

    std::optional<int> lcaInc =
        model_.LeastCommonAncestorInclude(leftInc, rightInc);

    std::optional<int> condArmId;
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

  int b = span.first, e = span.second;
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
  const int probe = b;

  std::vector<const RefoldModel::Segment*> hits;
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
    int sLen = s->e - s->b;
    int selLen = selected->e - selected->b;

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
  if (!m.invFile || m.GetInvB() < 0 || m.GetInvE() < 0)
    return false;

  // RefoldModel exposes directives; MacroDirective.subkind is expected to be
  // "define" based on your schema usage elsewhere.
  for (const auto &d : model_.GetMacroDirectives()) {
    if ("#define" != d.subkind)
      continue;
    if (d.sitePath.empty())
      continue;
    if (!m.invFile || *m.invFile != d.sitePath)
      continue;

    // If the invocation byte range lies within the #define's site range, treat
    // it as non-patchable.
    if (m.GetInvB() >= d.siteB && m.GetInvE() <= d.siteE)
      return true;
  }

  return false;
}

const RefoldModel::MacroInvocation *
RefoldEngine::SmallestCoveringPatchableMacro(int aStart, int aEnd) const {
  const RefoldModel::MacroInvocation *best = nullptr;
  int bestLen = std::numeric_limits<int>::max();

  for (const auto &m : model_.GetMacroInvocations()) {
    // Check if this macro covers the token range [aStart, aEnd)
    if (!m.Covers(aStart, aEnd))
      continue;

    // Must be patchable at a real call site.
    if (m.GetInvB() < 0 || m.GetInvE() < 0 || !m.invText)
      continue;

    // CRITICAL: never patch invocations that are spelled inside a #define
    // directive.
    if (IsInvocationInsideDefineDirective(m))
      continue;

    // Deterministic selection: smallest cover wins, ties broken by ID.
    int len = m.cover.end - m.cover.begin;
    if (!best || len < bestLen || (len == bestLen && m.id < best->id)) {
      best = &m;
      bestLen = len;
    }
  }

  return best;
}

const RefoldModel::IncludeItem *
RefoldEngine::SmallestCoveringInclude(int aLo, int aHi) const {
  trace("include/select",
        "ENTER smallestCoveringInclude(aLo={0}, aHi={1})  (isEmpty={2})", aLo,
        aHi, (aLo >= aHi));

  const RefoldModel::IncludeItem *best = nullptr;
  int bestWidth = std::numeric_limits<int>::max();
  const bool isEmpty = (aLo >= aHi);

  for (const auto &inc : model_.GetIncludes()) {
    debug("include/select",
          "  Considering include id={0} target={1} cover=[{2},{3})", inc.id,
          inc.target, inc.cover.begin, inc.cover.end);

    if (inc.cover.begin < 0 || inc.cover.end < 0) {
      debug("include/select", "    -> SKIP (invalid coverage)");
      continue;
    }

    bool covers;
    if (isEmpty) {
      // Zero-width insertion
      //
      // Treat both boundary gaps as belonging to the include:
      //   - gap at coverBegin (before the first PP token),
      //   - gap at coverEnd   (after the last PP token),
      // so we use <= on the right side instead of <.
      covers = (inc.cover.begin <= aLo) && (aLo <= inc.cover.end);
      debug("include/select",
            "    Zero-width check (inclusive end): ({0} <= {1}) && ({2} <= "
            "{3}) => {4}",
            inc.cover.begin, aLo, aLo, inc.cover.end, covers);
    } else {
      // Non-empty deletion/substitution range
      covers = (inc.cover.begin <= aLo) && (aHi <= inc.cover.end);
      debug("include/select",
            "    Non-empty check: ({0} <= {1}) && ({2} <= {3}) => {4}",
            inc.cover.begin, aLo, aHi, inc.cover.end, covers);
    }

    if (!covers) {
      debug("include/select", "    -> DOES NOT COVER (reject)");
      continue;
    }

    // Compute interval width
    int width = inc.cover.end - inc.cover.begin;
    debug("include/select", "    -> COVERS, width={0} (current bestWidth={1})",
          width, bestWidth);

    if (width < bestWidth) {
      debug("include/select", "       -> NEW BEST include id={0} (width={1})",
            inc.id, width);
      best = &inc;
      bestWidth = width;
    }
  }

  if (!best) {
    debug("include/select", "EXIT smallestCoveringInclude => NONE");
  } else {
    debug("include/select",
          "EXIT smallestCoveringInclude => include id={0} target={1} "
          "cover=[{2},{3})",
          best->id, best->target, best->cover.begin, best->cover.end);
  }

  return best;
}

const RefoldModel::MacroInvocation *
RefoldEngine::SmallestCoveringMacro(int aStart, int aEnd) const {
  const RefoldModel::MacroInvocation *best = nullptr;
  for (const auto &m : model_.GetMacroInvocations()) {
    int cb = m.cover.begin, ce = m.cover.end;
    if (cb < 0 || ce < 0)
      continue;
    if (cb <= aStart && aEnd <= ce) { // covers [aStart, aEnd)
      if (!best) {
        best = &m;
      } else {
        int len = ce - cb;
        int blen = best->cover.end - best->cover.begin;
        if (len < blen || (len == blen && m.id < best->id))
          best = &m;
      }
    }
  }
  return best;
}

bool RefoldEngine::HunkMapsToTU(int a0, int a1, StringRef tuPath) const {
  trace("tu/own", "hunkMapsToTU: check ownership for A[{0},{1}) tu={2}", a0, a1,
        tuPath);
  bool sawAnyTU = false;
  const auto &tokmapByPP = model_.GetTokmapByPP();
  for (int pp = a0; pp < a1; ++pp) {
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

  // Pure insertion (a0 == a1): decide based on the nearest mapped neighbors.
  // If both neighbors live in the same non-TU file, treat this as header-owned
  // so that we expand that include instead of forcing a TU edit at EOF.
  const RefoldModel::TokMapEntry *left = nullptr;
  for (int pp = a0 - 1; pp >= 0; --pp) {
    auto it = tokmapByPP.find(pp);
    if (it != tokmapByPP.end()) {
      left = &it->second;
      break;
    }
  }
  const RefoldModel::TokMapEntry *right = nullptr;
  // Ensure a0 is at least 0 before treating it as an unsigned index
  size_t startPP = (a0 < 0) ? 0 : static_cast<size_t>(a0);
  for (size_t pp = startPP; pp < tokmapByPP.size(); ++pp) {
    auto it = tokmapByPP.find(static_cast<int>(pp));
    if (it != tokmapByPP.end()) {
      right = &it->second;
      break;
    }
  }

  if (left && right) {
    // Both sides mapped. If they are the same non-TU file, it is header-owned.
    if (!PathsEqual(left->file, tuPath) && PathsEqual(left->file, right->file)) {
      trace("tu/own",
            "hunkMapsToTU: INSERT at pp={0} bracketed by same non-TU file "
            "left='{1}' right='{2}' (tu='{3}') -> header-owned",
            a0, left->file, right->file, tuPath);
      return false;
    }
  }

  // Single-sided cases: if the only neighbor we can see is non-TU,
  // conservatively treat this as non-TU so that the include/segment
  // logic gets a chance to own it.
  if (left && !PathsEqual(left->file, tuPath)) {
    trace("tu/own",
          "hunkMapsToTU: INSERT at pp={0} left neighbor is non-TU file='{1}' "
          "(tu='{2}'); right='{3}' -> header-owned",
          a0, left->file, tuPath, (right ? right->file : "<null>"));
    return false;
  }
  if (right && !PathsEqual(right->file, tuPath)) {
    trace("tu/own",
          "hunkMapsToTU: INSERT at pp={0} right neighbor is non-TU file='{1}' "
          "(tu='{2}'); left='{3}' -> header-owned",
          a0, right->file, tuPath, (left ? left->file : "<null>"));
    return false;
  }

  // Otherwise, either both neighbors are TU, or there are no neighbors at all.
  // In both situations we treat this as a TU-owned insertion and let tuByteSpan
  // place it using the deterministic neighbor-based insertion policy.
  return true;
}

std::optional<int>
RefoldEngine::AnchorToNearestSlotBoundaryFromPPGap(StringRef tuPath,
                                                   int ppGap) const {
  // Candidate record for potential anchor points
  struct Cand {
    int pp; // PP coordinate for the boundary
    int b;  // TU byte coordinate (possibly adjusted)
    const RefoldModel::Slot *slot;

    Cand(int pp, int b, const RefoldModel::Slot *slot)
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
  auto adjustSlot = [&tuText](const RefoldModel::Slot *s) -> int {
    int b = s->b;

    bool needsAdjustment =
        StringSwitch<bool>(s->kind)
            .Cases("after_include", "after_last_include", "arm_end", true)
            .Default(false);
    if (needsAdjustment)
      return b;

    if (b < 0 || (size_t)b >= tuText.size())
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
  for (const auto &s : model_.FindSlots(tuPath.str(), std::nullopt,
                                        std::nullopt, std::nullopt)) {
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

  // 2) Include directive boundaries
  for (const auto &inc : model_.GetIncludes()) {
    if (tuPath != inc.sitePath)
      continue;

    int incBeginPP = MinPPBegin(inc.spans);
    int incEndPP = MaxPPEnd(inc.spans);

    if (incBeginPP >= 0) {
      if (auto s = model_.GetBeforeIncludeSlot(inc.id))
        cands.emplace_back(incBeginPP, adjustSlot(*s), *s);
    }
    if (incEndPP >= 0) {
      if (auto s = model_.GetAfterIncludeSlot(inc.id))
        cands.emplace_back(incEndPP, adjustSlot(*s), *s);
    }
  }

  // 3) Conditional arm boundaries
  for (const auto &g : model_.GetConds()) {
    if (tuPath != g.file)
      continue;
    for (const auto &a : g.arms) {
      if (!a.ppSpan.has_value())
        continue;

      int armBeginPP = a.ppSpan->begin;
      int armEndPP = a.ppSpan->end;

      if (armBeginPP >= 0) {
        if (auto s = model_.GetArmBeginSlot(a.id))
          cands.emplace_back(armBeginPP, adjustSlot(*s), *s);
      }
      if (armEndPP >= 0) {
        if (auto s = model_.GetArmEndSlot(a.id))
          cands.emplace_back(armEndPP, adjustSlot(*s), *s);
      }
    }
  }

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
  auto getPriority = [](StringRef kind) -> int {
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
    int pc = getPriority(c->slot->kind);
    int pb = getPriority(best->slot->kind);

    // Tie-break: Priority -> Byte Offset -> Slot ID
    if (pc < pb || (pc == pb && c->b < best->b) ||
        (pc == pb && c->b == best->b && c->slot->id < best->slot->id)) {
      best = c;
    }
  }

  return best ? std::optional<int>(best->b) : std::nullopt;
}

std::pair<int, int>
RefoldEngine::TUByteSpan(int a0, int a1, StringRef tuPath) const {
  if (a0 > a1)
    std::swap(a0, a1);

  const bool isEmpty = (a0 == a1);
  const auto &tokmapByPP = model_.GetTokmapByPP();

  // For pure insertions, first prefer an explicit slot boundary (file_begin,
  // before_include, after_include, arm_begin, arm_end, file_end, ...).
  if (isEmpty) {
    if (std::optional<int> slotAnchor =
            AnchorToNearestSlotBoundaryFromPPGap(tuPath, a0)) {
      return {*slotAnchor, *slotAnchor};
    }
  }

  // Non-empty: compute min/max over TU-mapped subset only.
  int minB = std::numeric_limits<int>::max();
  int maxE = std::numeric_limits<int>::min();
  bool foundTuToken = false;

  for (int i = a0; i < a1; ++i) {
    auto it = tokmapByPP.find(i);
    if (it == tokmapByPP.end())
      continue;

    const auto &ent = it->second;
    if (ent.file.empty() || ent.file != tuPath)
      continue;
    if (ent.b < 0 || ent.e < 0)
      continue;

    if (ent.b < minB) minB = ent.b;
    if (ent.e > maxE) maxE = ent.e;
    foundTuToken = true;
  }

  if (foundTuToken) {
    return {minB, maxE};
  }

  // Empty insertion with no slot anchor: only anchor to a TU neighbor token
  // when the insertion is TU-owned. If either neighbor is in a header/include,
  // we must NOT fabricate a TU interval (that causes include insertions to snap
  // to TU boundaries).
  if (isEmpty) {
    // Find closest mapped neighbors.
    const RefoldModel::TokMapEntry *left = nullptr;
    for (int i = a0 - 1; i >= 0; --i) {
      auto it = tokmapByPP.find(i);
      if (it != tokmapByPP.end() && !it->second.file.empty() &&
          it->second.b >= 0 && it->second.e >= 0) {
        left = &it->second;
        break;
      }
    }

    const RefoldModel::TokMapEntry *right = nullptr;
    const int ppCount = model_.GetTokensCountA();
    for (int i = a0; i < ppCount; ++i) {
      auto it = tokmapByPP.find(i);
      if (it != tokmapByPP.end() && !it->second.file.empty() &&
          it->second.b >= 0 && it->second.e >= 0) {
        right = &it->second;
        break;
      }
    }

    // If both neighbors exist and are in the same non-TU file, this gap is
    // clearly inside that header/include; return no TU span.
    if (left && right && left->file != tuPath && left->file == right->file) {
      return {-1, -1};
    }

    // If either neighbor is a non-TU file, treat as header/arm-owned.
    if ((left && left->file != tuPath) || (right && right->file != tuPath)) {
      return {-1, -1};
    }

    // Otherwise, anchor to the nearest TU neighbor.
    if (right && right->file == tuPath) {
      return {right->b, right->b};
    }
    if (left && left->file == tuPath) {
      return {left->e, left->e};
    }
  }

  // No TU tokens in [a0,a1) (and no safe TU insertion anchor).
  return {-1, -1};
}

const RefoldModel::IncludeItem *
RefoldEngine::BoundaryParentIncludeForPureInsertion(const diffutils::Hunk &h,
                                                    StringRef tuPath) const {
  // We only care about pure insertions: A is empty, B is non-empty.
  if (h.aStart != h.aEnd || h.bStart >= h.bEnd) {
    return nullptr;
  }

  const int aPos = h.aStart;
  const int maxPP = model_.GetTokensCountA();

  // Find the nearest concrete tokens on the left / right in PP space.
  const auto *leftEntry =
      FindNearestTokmapEntry(std::min(aPos - 1, maxPP), -1, maxPP);
  const auto *rightEntry =
      FindNearestTokmapEntry(std::min(aPos, maxPP), 1, maxPP);

  std::optional<int> leftIncId =
      leftEntry ? model_.InnermostIncludeAtPP(leftEntry->pp) : std::nullopt;
  std::optional<int> rightIncId =
      rightEntry ? model_.InnermostIncludeAtPP(rightEntry->pp) : std::nullopt;

  // If both sides resolve to the same include (or both TU), this is not a
  // boundary between siblings – let the normal owner logic handle it.
  if (leftIncId == rightIncId) {
    return nullptr;
  }

  // Otherwise, attach the insertion to the lowest common ancestor include.
  // If either side is TU (nullopt) or they meet only at TU, LCA is nullopt
  // and the TU owns the insertion (caller will treat as TU-level hunk).
  std::optional<int> parentId =
      model_.LeastCommonAncestorInclude(leftIncId, rightIncId);
  if (!parentId) {
    return nullptr; // TU-owned
  }

  const RefoldModel::IncludeItem *parent = model_.GetIncludeById(*parentId);
  if (!parent) {
    return nullptr;
  }

  debug("owner",
        "pure-ins boundary at A[{0}]: leftInc={1} rightInc={2} parent={3}",
        aPos, leftIncId, rightIncId, parentId);

  return parent;
}

// ==================== Patch builders (include & macro) ====================

bool RefoldEngine::MacroExpansionEnvelopeB(
    const RefoldModel::MacroInvocation &m, bool onlyInvFile, int &begin,
    int &end) const {
  const auto &tokMapByPP = model_.GetTokmapByPP();
  if (tokMapByPP.empty())
    return false;

  int lo = std::numeric_limits<int>::max();
  int hi = std::numeric_limits<int>::min();
  bool any = false;

  auto addRange = [&](int l, int h) {
    for (int pp = l; pp < h; ++pp) {
      auto it = tokMapByPP.find(pp);
      if (it == tokMapByPP.end())
        continue;

      const auto &t = it->second;

      if (onlyInvFile) {
        if (t.file.empty() || t.file != m.invFile)
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
    const RefoldModel::MacroInvocation &m, int argIdx, StringRef baseArg,
    StringRef newArg, ArrayRef<int> a2b, const diffutils::Hunk &hintHunk,
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

        auto bEnv =
            MapAToBTokenEnvelopeStrictWithHint(a2b, s.begin, s.end, hintHunk);
        if (!bEnv)
          return false;

        StringRef tok = SliceBSource(bEnv->first, bEnv->second).trim();
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

  enum PasteType : unsigned {
    Unknown,
    Prefix,
    Suffix,
    Whole,
    Ambiguous
  };

  // Determine whether token pasting consumes a prefix/suffix/whole segment of
  // this argument: 0=none/unknown, 1=prefix, 2=suffix, 3=whole, 4=ambiguous
  PasteType pasteConsume = PasteType::Unknown;
  for (const auto &ps : m.pasteSpans) {
    if (ps.argIdx != argIdx)
      continue;

    StringRef aTokText = SliceASource(ps.begin, ps.end).trim();
    if (aTokText.empty())
      continue;

    if (ps.byteBegin < 0 || ps.byteEnd < ps.byteBegin ||
        static_cast<size_t>(ps.byteEnd) > aTokText.size())
      continue;

    StringRef segA = aTokText.substr(ps.byteBegin, ps.byteEnd - ps.byteBegin);
    if (segA.empty())
      continue;

    bool starts = baseTrim.starts_with(segA);
    bool ends = baseTrim.ends_with(segA);

    PasteType dir;
    if (baseTrim == segA)
      dir = PasteType::Whole;
    else if (starts && !ends)
      dir = PasteType::Prefix;
    else if (ends && !starts)
      dir = PasteType::Suffix;
    else if (starts && ends)
      dir = PasteType::Ambiguous;
    else
      dir = PasteType::Unknown;

    if (dir == PasteType::Unknown)
      continue;

    if (pasteConsume == PasteType::Unknown)
      pasteConsume = dir;
    else if (pasteConsume != dir)
      pasteConsume = PasteType::Ambiguous;
  }

  // Verify all standard (non-paste) occurrences.
  for (const auto &s : m.argSpans) {
    if (s.argIdx != argIdx)
      continue;

    if (s.kind != RefoldModel::PPArgSpanKind::Standard)
      continue;

    // Best-effort envelope. For now this is equivalent to StrictWithHint;
    // it exists as a semantic marker for call sites that want a best-effort
    // mapping.
    auto bEnv =
        MapAToBTokenEnvelopeStrictWithHint(a2b, s.begin, s.end, hintHunk);
    if (!bEnv || bEnv->first < 0 || bEnv->second <= bEnv->first)
      return false;

    StringRef tokText = SliceBSource(bEnv->first, bEnv->second).trim();
    if (tokText.empty())
      return false;

    bool ok;
    if (pasteConsume == PasteType::Suffix) {
      // Suffix segment is consumed by pasting; standard expansion is the
      // prefix.
      ok = argTrim.starts_with(tokText);
    } else if (pasteConsume == PasteType::Prefix) {
      // Prefix segment is consumed by pasting; standard expansion is the
      // suffix.
      ok = argTrim.ends_with(tokText);
    } else {
      ok = (tokText == argTrim);
    }

    if (!ok)
      return false;
  }

  if (argIsStringified)
    return true;

  // Paste-span verification is optional for callers that validate paste-token
  // correctness as a group (e.g., multi-span paste edits). When disabled, we
  // only validate standard+stringify occurrences above.
  if (!checkPasteSpans)
    return true;

  // If the current hunk does not touch any paste token, skip verifying paste
  // spans to avoid false negatives when the A->B token mapping is incomplete
  // away from the edit site.
  if (!HunkTouchesAnyPasteToken(m, hintHunk))
    return true;

  // Verify all paste-span occurrences.
  for (const auto &ps : m.pasteSpans) {
    if (ps.argIdx != argIdx)
      continue;

    auto bEnv = MapAToBTokenEnvelopePasteStrictWithHint(a2b, ps.begin, ps.end,
                                                        hintHunk);
    if (!bEnv || bEnv->first < 0 || bEnv->second <= bEnv->first)
      return false;

    if (bEnv->second - bEnv->first != 1)
      return false;

    StringRef aTokText = SliceASource(ps.begin, ps.end).trim();
    StringRef bTokText = SliceBSource(bEnv->first, bEnv->second).trim();

    if (ps.byteBegin < 0 || ps.byteEnd < ps.byteBegin ||
        static_cast<size_t>(ps.byteEnd) > aTokText.size())
      return false;

    StringRef oldSeg = aTokText.substr(ps.byteBegin, ps.byteEnd - ps.byteBegin);

    int delta =
        static_cast<int>(bTokText.size()) - static_cast<int>(aTokText.size());
    int bbB = ps.byteBegin;
    int beB = ps.byteEnd + delta;
    if (bbB < 0 || beB < bbB || static_cast<size_t>(beB) > bTokText.size())
      return false;

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

    bool ok;
    if (baseTrim == oldSeg) {
      ok = (argTrim == segB);
    } else if (starts && !ends) {
      ok = argTrim.starts_with(segB);
    } else if (ends && !starts) {
      ok = argTrim.ends_with(segB);
    } else {
      // Ambiguous or unclassifiable: refuse args-only.
      ok = false;
    }

    if (!ok)
      return false;
  }

  return true;
}

bool RefoldEngine::HunkTouchesAnyPasteToken(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h) {
  if (m.pasteSpans.empty())
    return false;

  const int a0 = h.aStart;
  const int a1 = h.aEnd;

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
                                 const diffutils::Hunk &h,
                                 ArrayRef<int> a2b) const {
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
  auto bEnvOpt = MapAToBTokenEnvelopePasteStrictWithHint(a2b, tokenSpan->begin,
                                                         tokenSpan->end, h);
  if (!bEnvOpt || bEnvOpt->first < 0 || bEnvOpt->second <= bEnvOpt->first)
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

  int diffStart = static_cast<int>(pref);
  int diffEndA = static_cast<int>(aLen - suff);

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
    if (ps->byteBegin < 0 || ps->byteEnd < ps->byteBegin)
      continue;

    bool hit;
    if (diffStart == diffEndA) {
      // Pure insertion/deletion at a point (no width in A). Treat as
      // overlapping if the point lies strictly inside the candidate slice.
      hit = (ps->byteBegin <= diffStart) && (diffStart < ps->byteEnd);
    } else {
      // General overlap between [diffStart,diffEndA) and
      // [ps.byteBegin,ps.byteEnd).
      int lo = std::max(ps->byteBegin, diffStart);
      int hi = std::min(ps->byteEnd, diffEndA);
      hit = hi > lo;
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
  int bb = chosen->byteBegin;
  int be = chosen->byteEnd;

  if (bb < 0 || be < bb || static_cast<size_t>(be) > aTok.size())
    return std::nullopt;

  // Compute the corresponding segment coordinates in the B pasted token.
  //
  // We assume the token-level edit does not permute the contribution
  // boundaries; instead, the chosen segment grows/shrinks by the overall token
  // length delta (bTokLen - aTokLen). This allows us to map [bb,be) in A to
  // [bb,be+delta) in B.
  int delta = static_cast<int>(bTok.size()) - static_cast<int>(aTok.size());
  int bbB = bb;
  int beB = be + delta;
  if (bbB < 0 || beB < bbB || static_cast<size_t>(beB) > bTok.size())
    return std::nullopt;

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
                                  const diffutils::Hunk &h,
                                  ArrayRef<int> a2b) const {
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

  // All candidates should reference the same pasted token range [begin, end) in A.
  const auto *tokenSpan = cands[0];

  auto bEnvOpt = MapAToBTokenEnvelopePasteStrictWithHint(a2b, tokenSpan->begin,
                                                         tokenSpan->end, h);
  if (!bEnvOpt || bEnvOpt->first < 0 || bEnvOpt->second <= bEnvOpt->first)
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
    return p1->byteBegin < p2->byteBegin;
  });

  std::optional<std::vector<std::string>> newSegs =
      SegmentPastedTokenArgsByFixedSlices(aTok, bTok, spans);
  if (!newSegs)
    return std::nullopt;

  std::vector<PasteArgEdit> edits;
  DenseSet<int> seenArgIdx;

  for (size_t i = 0; i < spans.size(); ++i) {
    const auto *ps = spans[i];
    if (ps->byteBegin < 0 || ps->byteEnd < ps->byteBegin)
      return std::nullopt;

    size_t bb = static_cast<size_t>(ps->byteBegin);
    size_t be = static_cast<size_t>(ps->byteEnd);
    if (be > aTok.size())
      return std::nullopt;

    StringRef oldSeg = aTok.substr(bb, be - bb);
    const std::string &newSeg = (*newSegs)[i];

    if (oldSeg == newSeg)
      continue;

    // If the same argument index appears twice in the same pasted token with
    // different edits, it's ambiguous.
    if (!seenArgIdx.insert(ps->argIdx).second)
      return std::nullopt;

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
    if (ps->byteBegin < 0 || ps->byteEnd < ps->byteBegin)
      return std::nullopt;
    if (static_cast<size_t>(ps->byteEnd) > aTok.size())
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
    ArrayRef<const RefoldModel::PPArgSpan *> spansAsc, int idx, int posA,
    int posB, MutableArrayRef<std::string> out) {
  if (static_cast<size_t>(idx) >= spansAsc.size()) {
    // All spans emitted; remaining fixed tail must match exactly.
    StringRef tail = aTok.substr(posA);
    return bTok.substr(posB).starts_with(tail) &&
           (posB + tail.size() == bTok.size());
  }

  const auto *ps = spansAsc[idx];
  if (ps->byteBegin < posA)
    return false;

  // Match the fixed slice before this span.
  StringRef fixedBefore = aTok.substr(posA, ps->byteBegin - posA);
  if (!bTok.substr(posB).starts_with(fixedBefore))
    return false;

  int argStartB = posB + static_cast<int>(fixedBefore.size());
  int nextPosA = ps->byteEnd;

  // Determine the fixed slice after this span (up to the next span, or the
  // tail).
  StringRef fixedAfter;
  if (static_cast<size_t>(idx + 1) < spansAsc.size()) {
    const auto *next = spansAsc[idx + 1];
    if (next->byteBegin < ps->byteEnd)
      return false;
    fixedAfter = aTok.substr(ps->byteEnd, next->byteBegin - ps->byteEnd);
  } else {
    fixedAfter = aTok.substr(ps->byteEnd);
  }

  // If there is no fixed anchor after this span and the total length changed,
  // we cannot determine the B segment boundary for this arg.
  if (fixedAfter.empty() && (static_cast<size_t>(idx + 1) < spansAsc.size()) &&
      (aTok.size() != bTok.size()))
    return false;

  if (fixedAfter.empty()) {
    // If this is the final arg span and there is no fixed tail, the arg's
    // contribution may legally grow or shrink. In that case, consume the
    // remainder of the B token.
    //
    // This is the common case for token-paste macros like:
    //   CONCAT(X, Y, Z) -> X##_##Y##_##Z
    // where the last argument is immediately followed by the end of the pasted
    // token.
    if (static_cast<size_t>(idx + 1) == spansAsc.size()) {
      out[idx] = bTok.substr(argStartB).str();
      return SegmentPastedTokenArgsByFixedSlicesRec(
          aTok, bTok, spansAsc, idx + 1, nextPosA,
          static_cast<int>(bTok.size()), out);
    }

    // Length-stable adjacent spans: use the A span length as the B span length.
    int aLen = ps->byteEnd - ps->byteBegin;
    int argEndB = argStartB + aLen;
    if (static_cast<size_t>(argEndB) > bTok.size())
      return false;

    out[idx] = bTok.substr(argStartB, aLen).str();
    return SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc, idx + 1,
                                                  nextPosA, argEndB, out);
  }

  // Search for the fixedAfter anchor in B at/after argStartB. We may have
  // multiple candidates if fixedAfter appears inside an arg; backtrack
  // deterministically.
  size_t k = bTok.find(fixedAfter, argStartB);
  while (k != StringRef::npos) {
    int currentK = static_cast<int>(k);
    out[idx] = bTok.substr(argStartB, currentK - argStartB).str();
    if (SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc, idx + 1,
                                               nextPosA, currentK, out)) {
      return true;
    }

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
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &hintHunk,
    ArrayRef<int> a2b, StringRef baseInvocationText,
    ArrayRef<std::pair<int, int>> invArgRanges,
    const DenseMap<int, std::string> &replByArgIdx) const {
  if (m.pasteSpans.empty())
    return true;

  // Precompute the original (base) spelling text for each argument we are
  // proposing to replace. We need this to derive a stable mapping from
  // "argument replacement" -> "paste segment update".
  DenseMap<int, std::string> baseArgByIdx;
  for (const auto &entry : replByArgIdx) {
    int argIdx = entry.first;
    if (argIdx < 0 || static_cast<size_t>(argIdx) >= invArgRanges.size())
      return false;

    auto range = invArgRanges[argIdx];
    StringRef rawArg =
        baseInvocationText.substr(range.first, range.second - range.first);
    baseArgByIdx[argIdx] = rawArg.trim().str();
  }

  // Group paste spans by the specific pasted-token occurrence they contribute
  // to. The grouping key is the A token interval [beginTok,endTok) of the
  // pasted token. In practice, paste spans are expected to describe a single
  // token, so (endTok - beginTok) should be 1.
  std::vector<std::pair<int, int>> tokenOrder;
  DenseMap<std::pair<int, int>, std::vector<RefoldModel::PPArgSpan>> spansByTok;
  for (const auto &ps : m.pasteSpans) {
    std::pair<int, int> key = {ps.begin, ps.end};
    if (spansByTok.find(key) == spansByTok.end()) {
      tokenOrder.push_back(key);
    }
    spansByTok[key].push_back(ps);
  }

  // For each pasted-token occurrence, simulate applying the per-arg
  // replacements to its sub-token argument segments and compare against the
  // edited B token spelling.
  for (const auto &key : tokenOrder) {
    int beginTok = key.first;
    int endTok = key.second;

    // We only support pasted-token occurrences that correspond to exactly one
    // token in A.
    if (endTok - beginTok != 1)
      return false;

    // Map the A pasted-token occurrence to a single B token envelope. This
    // mapping is paste-aware because token-paste edits often cause the pasted
    // token to be unmapped in a2b (-1).
    auto bEnv = MapAToBTokenEnvelopePasteStrictWithHint(a2b, beginTok, endTok,
                                                        hintHunk);
    if (!bEnv || (bEnv->second - bEnv->first != 1))
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
    std::sort(spans.begin(), spans.end(), [](const auto &p1, const auto &p2) {
      return p1.byteBegin > p2.byteBegin;
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

      // The paste span must define a valid character slice inside the A
      // pasted-token spelling.
      if (ps.byteBegin < 0 || ps.byteEnd < ps.byteBegin ||
          static_cast<size_t>(ps.byteEnd) > aTok.size())
        return false;

      // Extract the original pasted-token segment contributed by this argument.
      StringRef oldSeg =
          StringRef(aTok).substr(ps.byteBegin, ps.byteEnd - ps.byteBegin);

      // Derive the new pasted-token segment from the argument replacement. This
      // is intentionally conservative and must be deterministic; if we cannot
      // derive a segment safely, fail.
      StringRef newSeg =
          DeriveNewPasteSegmentFromSpellingReplacement(baseArg, newArg, oldSeg);
      if (newSeg.data() == nullptr) // Check for "null" StringRef
        return false;

      // Rewrite only the identified segment region inside the synthetic pasted-
      // token spelling.
      expected =
          stringutils::replaceRange(expected, ps.byteBegin, ps.byteEnd, newSeg);
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

std::optional<RefoldEngine::MacroPatch>
RefoldEngine::BuildMacroInvocationPatchArgsOnly(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    ArrayRef<int> a2b, StringRef baseInvText) const {
  // We can only emit an invocation patch if the producer provided a concrete
  // byte range.
  if (m.GetInvB() < 0 || m.GetInvE() < 0)
    return std::nullopt;

  trace("macro/args", "args-only? inv id={0} name={1} {2} baseInv={3}", m.id,
        m.name, h, stringutils::showWSWithClip(baseInvText, 200));

  // Parse the byte ranges for each argument's "content" within the invocation
  // spelling. These ranges are later used to splice per-arg replacements back
  // into the invocation text.
  auto rangesOpt = ParseMacroInvocationArgContentRanges(baseInvText);
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
    auto edits = DerivePasteArgEdits(m, h, a2b);
    if (edits && !edits->empty()) {
      DenseMap<int, std::string> replByArgIdx;
      for (const auto &pae : *edits) {
        int argIdx = pae.argIdx;
        if (argIdx < 0 || static_cast<size_t>(argIdx) >= invArgRanges.size())
          return std::nullopt;

        // Reject multiple independent edits to the same arg index inside one
        // pasted token (this can be generalized later, but keeping it strict
        // avoids ambiguous splice order).
        if (replByArgIdx.count(argIdx))
          return std::nullopt;

        auto range = invArgRanges[argIdx];
        StringRef baseArgText =
            baseInvText.substr(range.first, range.second - range.first);

        // Splice the sub-token replacement into the spelling arg
        // conservatively.
        std::string newArg = SplicePasteSegmentIntoSpellingArg(
            baseArgText, pae.oldSeg, pae.newSeg);
        if (newArg.empty())
          return std::nullopt;

        // Per-arg safety gate: validate standard + stringify occurrences for
        // this arg.
        //
        // NOTE: For multi-span paste edits where the pasted token length may
        // change, per-arg paste-span validation cannot be done reliably in
        // isolation. We validate paste tokens as a *group* below via
        // pasteArgReplacementsMatchAllPasteTokensInB(...).
        if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
                m, argIdx, baseArgText, newArg, a2b, h)) {
          return std::nullopt;
        }

        replByArgIdx[argIdx] = std::move(newArg);
      }

      if (!replByArgIdx.empty()) {
        // Combined safety gate: applying all derived replacements must
        // reconstruct every pasted token occurrence exactly as seen in B.
        if (!PasteArgReplacementsMatchAllPasteTokensInB(
                m, h, a2b, baseInvText, invArgRanges, replByArgIdx)) {
          return std::nullopt;
        }

        // Apply all replacements to the invocation string (descending order).
        std::string newInv = baseInvText.str();
        auto keys = llvm::to_vector<8>(
            llvm::map_range(replByArgIdx, [](auto &e) { return e.first; }));
        std::sort(keys.begin(), keys.end(), [&](int a, int b) {
          return invArgRanges[a].first > invArgRanges[b].first;
        });

        for (int argIdx : keys) {
          auto r = invArgRanges[argIdx];
          newInv = stringutils::replaceRange(newInv, r.first, r.second,
                                             replByArgIdx[argIdx]);
        }

        trace("macro/args", "  args-only SUCCESS newInv='{0}'",
              stringutils::showWSWithClip(newInv, 200));
        return MacroPatch{m.GetInvB(), m.GetInvE(), std::move(newInv)};
      }
    }

    // Single-segment paste edit (existing behavior)
    //
    // This handles the common case where only one pasted segment changes (e.g.
    // X##_##Y, changing just X). The multi-span derivation above requires token
    // lengths to remain stable; when they do not, we fall back to deriving a
    // single segment edit from the token-level diff.
    auto pae = DerivePasteArgEdit(m, h, a2b);
    if (pae) {
      int argIdx = pae->argIdx;

      // HARD FAILURE: If we derived a paste edit but the index is invalid,
      // we must exit, not fall through.
      if (argIdx < 0 || static_cast<size_t>(argIdx) >= invArgRanges.size())
        return std::nullopt;

      auto r = invArgRanges[argIdx];
      StringRef baseArgText = baseInvText.substr(r.first, r.second - r.first);
      std::string newArg = SplicePasteSegmentIntoSpellingArg(
          baseArgText, pae->oldSeg, pae->newSeg);
      if (newArg.empty())
        return std::nullopt;

      // Safety gate: for single-segment paste edits we can directly validate
      // all occurrences, including paste-span occurrences, against the B
      // stream.
      if (!MacroArgReplacementMatchesAllOccurrencesInB(m, argIdx, baseArgText,
                                                       newArg, a2b, h))
        return std::nullopt;

      std::string newInv =
          stringutils::replaceRange(baseInvText, r.first, r.second, newArg);
      trace("macro/args", "  args-only SUCCESS newInv='{0}'",
            stringutils::showWSWithClip(newInv, 200));
      return MacroPatch{m.GetInvB(), m.GetInvE(), std::move(newInv)};
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
  if (!HunkFullyWithinArgSpans(h, occs, touched)) {
    trace("macro/args",
          "  hunk not fully within any arg spans -> fail args-only");
    return std::nullopt;
  }

  trace("macro/args", "  touched={0}", stringutils::boolArrayToString(touched));

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.
  DenseMap<int, std::string> replByArgIdx;
  for (size_t i = 0; i < occs.size(); ++i) {
    if (!touched[i])
      continue;

    const auto &sp = occs[i];
    int argIdx = sp.argIdx;
    if (argIdx < 0 || static_cast<size_t>(argIdx) >= invArgRanges.size())
      return std::nullopt;

    // Base spelling for this argument in the invocation text (used for splice
    // and consistency).
    auto r0 = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r0.first, r0.second - r0.first);

    // Map the A occurrence envelope to B using the alignment map. For
    // paste-kind spans we use the paste-aware envelope mapping; otherwise the
    // standard strict envelope mapping.
    auto bEnv =
        (sp.kind == RefoldModel::PPArgSpanKind::Paste)
            ? MapAToBTokenEnvelopePasteStrictWithHint(a2b, sp.begin, sp.end, h)
            : MapAToBTokenEnvelopeStrictWithHint(a2b, sp.begin, sp.end, h);
    if (!bEnv) {
      // Conservative fallback: use the hunk's B range if the span mapping
      // fails. If the hunk doesn't have a valid B range either, we cannot
      // safely derive an args-only replacement.
      if (h.bStart < 0 || h.bEnd < 0 || h.bStart >= h.bEnd)
        return std::nullopt;
      bEnv = {h.bStart, h.bEnd};
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
                                                     newArg, a2b, h)) {
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
  // the same baseInvocationText.
  std::string finalInv = baseInvText.str();
  auto finalKeys = llvm::to_vector(
      llvm::map_range(replByArgIdx, [](auto &e) { return e.first; }));
  std::sort(finalKeys.begin(), finalKeys.end(), [&](int a, int b) {
    return invArgRanges[a].first > invArgRanges[b].first;
  });
  for (int argIdx : finalKeys) {
    auto r = invArgRanges[argIdx];
    finalInv = stringutils::replaceRange(finalInv, r.first, r.second,
                                         replByArgIdx[argIdx]);
  }

  return MacroPatch{m.GetInvB(), m.GetInvE(), std::move(finalInv)};
}

StringRef RefoldEngine::SliceSource(ArrayRef<size_t> tokOff, StringRef source,
                                    int startTok, int endTok) {
  if (tokOff.empty() || source.empty())
    return "";

  const int n = static_cast<int>(tokOff.size());

  // Clamp token indices to valid array bounds.
  int loTok = std::clamp(startTok, 0, n - 1);
  int hiTok = std::clamp(endTok, loTok, n - 1);

  int lo = tokOff[loTok];
  int hi = tokOff[hiTok];

  // Clamp byte offsets to the actual string length.
  const int sourceLen = static_cast<int>(source.size());
  lo = std::clamp(lo, 0, sourceLen);
  hi = std::clamp(hi, lo, sourceLen);

  return source.substr(lo, hi - lo);
}

std::optional<std::pair<int, int>>
RefoldEngine::MapAToBTokenEnvelopeStrict(ArrayRef<int> a2b, int aBegin,
                                         int aEnd) {
  if (a2b.empty())
    return std::nullopt;

  // Clamp and validate input range.
  if (aBegin < 0)
    aBegin = 0;
  if (aEnd > static_cast<int>(a2b.size()))
    aEnd = static_cast<int>(a2b.size());

  if (aBegin >= aEnd)
    return std::nullopt;

  // Case 1: nearest mapped neighbors bracket the span => exact envelope.
  // mapBackwardToB and mapForwardToB should return -1 on failure.
  int bLeft = MapBackwardToB(a2b, aBegin - 1);
  int bRight = MapForwardToB(a2b, aEnd);

  if (bLeft >= 0 && bRight >= 0 && bLeft < bRight) {
    return std::make_pair(bLeft + 1, bRight);
  }

  // Case 2: span contains mapped tokens => exact min/max envelope.
  int minB = std::numeric_limits<int>::max();
  int maxB = std::numeric_limits<int>::min();

  for (int a = aBegin; a < aEnd; ++a) {
    int b = a2b[a];
    if (b >= 0) {
      minB = std::min(minB, b);
      maxB = std::max(maxB, b);
    }
  }

  if (minB == std::numeric_limits<int>::max())
    return std::nullopt;

  return std::make_pair(minB, maxB + 1);
}

std::optional<std::pair<int, int>>
RefoldEngine::MapAToBTokenEnvelopeStrictWithHint(
    ArrayRef<int> a2b, int aBegin, int aEnd, const diffutils::Hunk &hintHunk) {
  std::optional<std::pair<int, int>> env =
      MapAToBTokenEnvelopeStrict(a2b, aBegin, aEnd);
  if (!env) {
    // No mapped tokens: only safe envelope is the hint hunk IF it overlaps the
    // A-span.
    if (hintHunk.bStart < 0 || hintHunk.bEnd <= hintHunk.bStart)
      return std::nullopt;

    if (hintHunk.aEnd <= aBegin || hintHunk.aStart >= aEnd)
      return std::nullopt;

    return std::make_pair(hintHunk.bStart, hintHunk.bEnd);
  }

  // If the hint hunk does not overlap the A-span, do not use it; return the
  // strict mapping.
  if (hintHunk.aEnd <= aBegin || hintHunk.aStart >= aEnd)
    return env;

  // Detect whether the A-span has unmapped tokens at the edges.
  int firstMappedA = -1;
  int lastMappedA = -1;

  int a2bSize = static_cast<int>(a2b.size());
  for (int a = std::max(0, aBegin); a < std::min(aEnd, a2bSize); ++a) {
    if (a2b[a] >= 0) {
      if (firstMappedA < 0)
        firstMappedA = a;
      lastMappedA = a;
    }
  }

  if (firstMappedA < 0) {
    // Should not happen if env is valid, but return strict mapping as fallback.
    return env;
  }

  bool missingLeft = false;
  bool missingRight = false;

  for (int a = aBegin; a < firstMappedA; ++a) {
    if (a >= 0 && a < a2bSize && a2b[a] < 0) {
      missingLeft = true;
      break;
    }
  }
  for (int a = lastMappedA + 1; a < aEnd; ++a) {
    if (a >= 0 && a < a2bSize && a2b[a] < 0) {
      missingRight = true;
      break;
    }
  }

  int b0 = env->first;
  int b1 = env->second;

  // Extend ONLY on sides where unmapped tokens exist by merging with the hint.
  if (missingLeft)
    b0 = std::min(b0, hintHunk.bStart);
  if (missingRight)
    b1 = std::max(b1, hintHunk.bEnd);

  // Ensure envelope is valid.
  if (b0 < 0)
    b0 = 0;
  if (b1 < b0)
    b1 = b0;

  return std::make_pair(b0, b1);
}

std::optional<std::pair<int, int>>
RefoldEngine::MapAToBTokenEnvelopePasteStrictWithHint(
    ArrayRef<int> a2b, int aBegin, int aEnd, const diffutils::Hunk &hintHunk) {
  // First try the strict token-mapping envelope (with an optional union with an
  // overlapping hunk when there is partial mapping).
  auto env = MapAToBTokenEnvelopeStrictWithHint(a2b, aBegin, aEnd, hintHunk);
  if (env)
    return env;

  // No mapped tokens exist for this A span. The only sound B envelope is the
  // hunk itself, but only if it overlaps the A range and provides a valid B
  // range.
  if (hintHunk.bStart < 0 || hintHunk.bEnd <= hintHunk.bStart)
    return std::nullopt;
  if (hintHunk.aEnd <= aBegin || hintHunk.aStart >= aEnd)
    return std::nullopt;

  // The only sound B envelope is the hunk itself.
  return std::make_pair(hintHunk.bStart, hintHunk.bEnd);
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
    MutableArrayRef<char> touched) {
  int a0 = h.aStart;
  int a1 = h.aEnd;

  // Insertion: attribute it to the arg span whose [begin,end] contains the
  // insertion point.
  if (a0 == a1) {
    trace("macro/debug", "Checking insertion at A={0}", a0);
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      trace("macro/debug", "  Arg {0} span: [{1}, {2}]", i, s.begin, s.end);
      if (a0 >= s.begin && a0 <= s.end) {
        touched[i] = 1;
        return true;
      }
    }
    trace("macro/debug", "  FAILED: Point {0} not in any span", a0);
    return false;
  }

  // Replacement/deletion: every covered token must fall inside some arg span.
  bool any = false;
  for (int a = a0; a < a1; ++a) {
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
    if (!inSome)
      return false;
  }
  return any;
}

std::optional<std::vector<std::pair<int, int>>>
RefoldEngine::ParseMacroInvocationArgContentRanges(StringRef invText) {
  size_t open = invText.find('(');
  if (open == StringRef::npos)
    return std::nullopt;

  std::vector<std::pair<int, int>> out;
  size_t n = invText.size();

  int depth = 0;
  bool inS = false;
  bool inD = false;

  size_t argStart = open + 1;
  for (size_t i = open + 1; i < n; i++) {
    char c = invText[i];

    if (inS) {
      if (c == '\\' && i + 1 < n) {
        i++;
        continue;
      }
      if (c == '\'')
        inS = false;
      continue;
    }
    if (inD) {
      if (c == '\\' && i + 1 < n) {
        i++;
        continue;
      }
      if (c == '"')
        inD = false;
      continue;
    }

    if (c == '\'') {
      inS = true;
      continue;
    }
    if (c == '"') {
      inD = true;
      continue;
    }

    if (c == '(' || c == '[' || c == '{') {
      depth++;
      continue;
    }

    if (c == ')') {
      if (depth == 0) {
        out.push_back(stringutils::trimWsRange(invText, argStart, i));
        return out;
      }
      depth--;
      continue;
    }

    if (c == ',' && depth == 0) {
      out.push_back(stringutils::trimWsRange(invText, argStart, i));
      argStart = i + 1;
    }
  }

  return std::nullopt;
}

RefoldEngine::IncludePatch
RefoldEngine::BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                         const diffutils::Hunk &h) const {
  // Extra debug: show the raw PP hunk slices.
  DebugIncludePatch("pre", inc, h);

  std::string insertBytes;

  // Validate hunk bounds against B-token offsets.
  if (h.bStart >= 0 && static_cast<size_t>(h.bStart) < bTokOff_.size() &&
      h.bEnd >= 0 && static_cast<size_t>(h.bEnd) < bTokOff_.size() &&
      h.bEnd >= h.bStart) {
    int b0 = bTokOff_[h.bStart];
    int b1 = bTokOff_[h.bEnd];

    // Clamp byte offsets to the actual length of bSource_ (defensively handle
    // huge sizes).
    const size_t sourceLen = bSource_.size();
    const int maxLen =
        (sourceLen > static_cast<size_t>(std::numeric_limits<int>::max()))
            ? std::numeric_limits<int>::max()
            : static_cast<int>(sourceLen);

    b0 = std::clamp(b0, 0, maxLen);
    b1 = std::clamp(b1, 0, maxLen);

    if (b1 >= b0) {
      insertBytes = bSource_.substr(b0, b1 - b0).str();
    }
  }

  IncludePatch patch{&inc,  std::move(insertBytes), h.aStart, h.aEnd, h.bStart,
                     h.bEnd};

  trace("include/patch",
        "built inc #{0} patch A[{1},{2})->B[{3},{4}) len(insertBytes)={5}",
        inc.id, patch.aStart, patch.aEnd, patch.bStart, patch.bEnd,
        patch.insertBytes.size());

  return patch;
}

RefoldEngine::MacroPatch RefoldEngine::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    ArrayRef<int> a2b, StringRef baseInvText,
    const DenseMap<int, DenseMap<int, MacroPatch>> &patchMap) const {
  debug("macro/patch",
        "BEGIN inv id={0} name={1} invFile={2} invB/E=[{3},{4}] "
        "coverA=[{5},{6}) hunkB=[{7},{8})",
        m.id, m.name, m.invFile, m.GetInvB(), m.GetInvE(), m.cover.begin,
        m.cover.end, h.bStart, h.bEnd);

  // 1. BAKE IN INNER MACRO EDITS
  ///////////////////////////////
  // If this macro's arguments contain other macros that have already been
  // patched, we need to update our base invocation text to reflect those
  // "inner" changes.
  std::string updatedInvText = baseInvText.str();
  const int fileLen = static_cast<int>(bSource_.size());

  for (const auto &arg : m.argSpans) {
    // Find the smallest macro covering this specific argument's token range.
    const RefoldModel::MacroInvocation *innerM =
        SmallestCoveringMacro(arg.begin, arg.end);

    // If an inner macro exists and it's not the current one, look for its
    // patch.
    if (innerM && innerM->id != m.id) {
      auto ownerIt = patchMap.find(
          innerM->ownerIncludeId ? *innerM->ownerIncludeId : kNoOwner);
      if (ownerIt != patchMap.end()) {
        auto patchIt = ownerIt->second.find(innerM->id);
        if (patchIt != ownerIt->second.end()) {
          const std::string &innerReplacement = patchIt->second.replacement;

          // Map the preprocessed token indices to global file byte offsets.
          int gStart =
              ByteStartForPPInFile(m.invFile ? *m.invFile : "", arg.begin,
                                   /*fallbackToEOF*/ false, fileLen);
          int gEnd =
              ByteEndForPPInFile(m.invFile ? *m.invFile : "", arg.end - 1,
                                 /*fallbackToEOF*/ false, fileLen);

          if (gStart != -1 && gEnd != -1) {
            // Convert global offsets to local offsets relative to macro
            // invocation start.
            int localStart = gStart - m.GetInvB();
            int localEnd = gEnd - m.GetInvB();

            if (localStart >= 0 && m.invText &&
                static_cast<size_t>(localEnd) <= m.invText->size()) {
              StringRef originalArgText =
                  StringRef(*m.invText)
                      .substr(localStart, localEnd - localStart);

              // Replace the original argument text with the already-computed
              // patch.
              size_t pos = updatedInvText.find(originalArgText);
              if (pos != std::string::npos) {
                updatedInvText.replace(pos, originalArgText.size(),
                                       innerReplacement);
              }
            }
          }
        }
      }
    }
  }

  // 2. CHECK FOR ARGS-ONLY CHANGES
  /////////////////////////////////
  // Attempt a "surgical" patch where we only replace specific arguments. This
  // is preferred over whole-cover replacement because it preserves the original
  // call-site formatting.
  auto argsOnly = BuildMacroInvocationPatchArgsOnly(m, h, a2b, updatedInvText);
  if (argsOnly)
    return *argsOnly;

  // 3. FALLBACK: MAP BYTE RANGES FROM B (The "Whole Cover" Approach)
  ///////////////////////////////////////////////////////////////////
  // If surgical patching fails, we try to project the entire macro expansion
  // from the B-stream.
  int bStartIdx = MapForwardToB(a2b, m.cover.begin);
  int bEndIdxEx = MapBackwardToB(a2b, m.cover.end - 1);

  // If mapping fails (e.g., token was deleted), check if the hunk itself
  // suggests deletion.
  if (bStartIdx < 0 || bEndIdxEx < 0 || bStartIdx > bEndIdxEx) {
    if (h.bStart >= h.bEnd) {
      return MacroPatch{m.GetInvB(), m.GetInvE(), ""};
    }
    // Use hunk coordinates as a last resort.
    bStartIdx = h.bStart;
    bEndIdxEx = h.bEnd - 1;
  }

  // Clamp indices to valid token offset ranges.
  int covLo = bStartIdx;
  int covHi = bEndIdxEx + 1;

  // Ensure the B envelope includes the edited region, even when the A->B
  // mapping drops mismatching tokens (e.g. edited stringified literals).
  if (h.bStart >= 0 && h.bEnd >= h.bStart) {
    covLo = std::min(covLo, h.bStart);
    covHi = std::max(covHi, h.bEnd);
  }

  // Clamp indices to valid token offset ranges.
  const int n = static_cast<int>(bTokOff_.size());
  covLo = std::clamp(covLo, 0, std::max(0, n - 2));
  covHi = std::clamp(covHi, covLo, std::max(covLo, n - 1));

  // Determine a "Strict" envelope: tokens that are definitively part of the
  // expansion.
  int envStrictLo = 0, envStrictHi = 0;
  bool haveEnvStrict =
      MacroExpansionEnvelopeB(m, true, envStrictLo, envStrictHi);
  int aLo = covLo, aHi = covHi;
  if (haveEnvStrict) {
    const int iLo = std::max(covLo, envStrictLo);
    if (iLo < covHi)
      aLo = iLo;
  }

  aLo = std::clamp(aLo, 0, std::max(0, n - 2));
  aHi = std::clamp(aHi, aLo, std::max(aLo, n - 1));
  StringRef candA;
  if (bTokOff_[aHi] > bTokOff_[aLo]) {
    candA = stringutils::trimEdgeSpaces(
        bSource_.substr(bTokOff_[aLo], bTokOff_[aHi] - bTokOff_[aLo]));
  } else {
    candA = "";
  }

  // Determine a "Union" envelope: includes adjacent tokens that might be
  // structural (like ';').
  int envAllLo = 0, envAllHi = 0;
  bool haveEnvAll = MacroExpansionEnvelopeB(m, false, envAllLo, envAllHi);
  int bLoTok = aLo, bHiTok = aHi;
  if (haveEnvAll) {
    bLoTok = std::min(aLo, envAllLo);
    bHiTok = std::max(aHi, envAllHi);
  }

  bLoTok = std::clamp(bLoTok, 0, std::max(0, n - 2));
  bHiTok = std::clamp(bHiTok, bLoTok, std::max(bLoTok, n - 1));
  StringRef candB;
  if (bTokOff_[bHiTok] > bTokOff_[bLoTok]) {
    candB = stringutils::trimEdgeSpaces(
        bSource_.substr(bTokOff_[bLoTok], bTokOff_[bHiTok] - bTokOff_[bLoTok]));
  } else {
    candB = "";
  }

  // RECONCILIATION LOGIC: Should we include the extra tokens from 'candB'? We
  // only choose 'candB' if the extra tokens are part of the macro's body and
  // the expansion ends with a semicolon (to prevent double-semicolons).
  bool chooseB = false;
  StringRef aT = candA.trim();
  StringRef bT = candB.trim();
  if (!aT.empty() && bT.ends_with(aT)) {
    auto inBody = [&](int pp) {
      for (const auto &s : m.bodySpans)
        if (s.IsValid() && pp >= s.begin && pp < s.end)
          return true;
      return false;
    };
    auto inArg = [&](int pp) {
      for (const auto &s : m.argSpans)
        if (s.IsValid() && pp >= s.begin && pp < s.end)
          return true;
      return false;
    };

    // Ensure all extra tokens are from the macro body, not from arguments.
    bool allExtraFromBodyNotArgs = true;
    for (int pp = std::min(bLoTok, aLo); pp < std::max(bLoTok, aLo); ++pp) {
      if (!inBody(pp) || inArg(pp)) {
        allExtraFromBodyNotArgs = false;
        break;
      }
    }

    // Check for a trailing semicolon in the expansion text.
    bool endsAtSemicolon = false;
    const int startByte = bTokOff_[bLoTok];
    int endByte = bTokOff_[bHiTok] - 1;
    while (endByte >= startByte) {
      char ch = bSource_[endByte];
      if (!stringutils::isWs(ch)) {
        endsAtSemicolon = (ch == ';');
        break;
      }
      --endByte;
    }
    chooseB = allExtraFromBodyNotArgs && endsAtSemicolon;
  }

  StringRef chosen = chooseB ? candB : candA;

  // 4. FINAL MERGE
  /////////////////
  // If the 'chosen' text looks like a standard call-site (contains the macro
  // name) but lacks the inner macro patches we calculated in Step 1, prefer
  // 'updatedInvText'.
  std::string finalResult = chosen.str();
  if (LooksLikeCallsiteText(chosen, m) && chosen != updatedInvText) {
    finalResult = updatedInvText;
  }

  return MacroPatch{m.GetInvB(), m.GetInvE(), std::move(finalResult)};
}

bool RefoldEngine::LooksLikeCallsiteText(
    StringRef text, const RefoldModel::MacroInvocation &m) {
  if (m.name.empty())
    return false;

  StringRef name = m.name;
  const bool funcLike = (m.subkind == "func");

  int textLen = static_cast<int>(text.size());
  int nameLen = static_cast<int>(name.size());
  int from = 0;

  while (from <= textLen - nameLen) {
    size_t i = text.find(name, from);
    if (i == StringRef::npos)
      break;

    int matchIdx = static_cast<int>(i);

    // Ensure identifier boundaries, so we do not match substrings inside other
    // identifiers.
    if (stringutils::isIdentBoundary(text, matchIdx - 1) &&
        stringutils::isIdentBoundary(text, matchIdx + nameLen)) {
      if (!funcLike)
        return true;

      int j = matchIdx + nameLen;
      while (j < textLen && stringutils::isWs(text[j]))
        j++;
      if (j < textLen && text[j] == '(')
        return true;
    }

    from = matchIdx + nameLen;
  }
  return false;
}

// =========== Include processing (normalize, materialize, apply) ===========

void RefoldEngine::MaterializeIncludeExpansion(
    int includeId, const DenseMap<int, IncludeEdits> &perInclude,
    const DenseMap<int, std::vector<MacroPatch>> &macroPatchesByOwner,
    const DenseMap<int, std::vector<const RefoldModel::IncludeItem *>>
        &children,
    DenseMap<int, std::string> &includeExpansion) const {
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

  auto hasDescendantWork = [&](auto &&self, int id) -> bool {
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
      const int n = static_cast<int>(bytes.size());
      const int siteStart = std::clamp(static_cast<int>(child->siteB), 0, n);
      const int siteEnd =
          std::clamp(static_cast<int>(child->siteE), siteStart, n);

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

  const int aLo = p.aStart;
  const int aHi = p.aEnd;

  const RefoldModel::HeaderDecl *bestCover = nullptr;
  const RefoldModel::HeaderDecl *bestOverlap = nullptr;

  auto getSpanLen = [](const RefoldModel::HeaderDecl *d) {
    return d->ppSpan.end - d->ppSpan.begin;
  };

  for (const auto &d : inc.decls) {
    const int dLo = d.ppSpan.begin;
    const int dHi = d.ppSpan.end;
    const int curSpanLen = dHi - dLo;

    // Pure insertion: treat as attached at aLo
    if (aLo == aHi) {
      // Special case: insertions at decl end are treated as inclusive
      const bool inside = (aLo >= dLo && aLo <= dHi);
      if (!inside)
        continue;

      if (!bestCover || curSpanLen < getSpanLen(bestCover))
        bestCover = &d;
      continue;
    }

    // Non-empty A-interval
    const bool covers = (aLo >= dLo && aHi <= dHi);
    const bool overlaps = (aLo < dHi && aHi > dLo);

    if (covers) {
      if (!bestCover || curSpanLen < getSpanLen(bestCover))
        bestCover = &d;
    } else if (overlaps) {
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

  const int fileLen = static_cast<int>(headerText.size());

  debug("include/apply",
        "ENTER computeIncludeTextEdits file={0} len={1} patches={2} "
        "cover=[{3},{4})",
        file, fileLen, ie.patches.size(), ie.include->cover.begin,
        ie.include->cover.end);

  // PP cover for this include inside the header; if not present, these will
  // already have been derived from spans when building the model.
  const int coverBegin = std::max(0, ie.include->cover.begin);
  const int coverEnd = std::max(coverBegin, ie.include->cover.end);

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
            "ppSpan=[{6},{7})",
            file, idx, decl->kind, decl->name, decl->headerB, decl->headerE,
            decl->ppSpan.begin, decl->ppSpan.end);
    } else {
      debug("include/apply", "file={0} patch[{1}] ownerDecl=<none>", file, idx);
    }

    // Effective PP coverage in this header we're allowed to touch.
    // For INSERTs we deliberately work at include scope so that inserts that
    // land exactly at a declaration boundary (for example, between
    // `int yyy(...);` and `int zzz(...);`) can anchor to the first token
    // of the following declaration rather than being forced back inside the
    // previous one.  For DELETE / REPLACE we restrict to the owning decl.
    int ppLo = coverBegin;
    int ppHi = coverEnd;
    if (!isInsert && decl) {
      ppLo = std::max(ppLo, decl->ppSpan.begin);
      ppHi = std::min(ppHi, decl->ppSpan.end);
    }
    ppHi = std::max(ppHi, ppLo);

    trace("include/apply",
          "file={0} patch[{1}] effectivePP=[{2},{3}) cover=[{4},{5})", file,
          idx, ppLo, ppHi, coverBegin, coverEnd);

    int startByte = -1;
    int endByte = -1;

    const auto &tokmapByPP = model_.GetTokmapByPP();
    if (isInsert) {
      // INSERT: interpret A-position as "before the next token" in this header.
      const int pos = p.aStart;
      int anchorPP = -1;

      // 1) Prefer the right neighbor: smallest pp >= pos in [ppLo, ppHi).
      for (int pp = std::max(pos, ppLo); pp < ppHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          anchorPP = pp;
          break;
        }
      }

      if (anchorPP >= 0) {
        // Insert immediately before the right neighbor token.
        startByte = ByteStartForPPInFile(file, anchorPP,
                                         /* fallbackToEOF */ false, fileLen);
        trace("include/apply",
              "file={0} patch[{1}] INSERT: right-neighbor anchorPP={2} -> "
              "startByte={3}",
              file, idx, anchorPP, startByte);
        if (startByte < 0) {
          continue;
        }
      } else {
        // 2) No right neighbor; fall back to the last left neighbor.
        for (int pp = std::min(pos - 1, ppHi - 1); pp >= ppLo; --pp) {
          auto it = tokmapByPP.find(pp);
          if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
            anchorPP = pp;
            break;
          }
        }

        if (anchorPP >= 0) {
          startByte = ByteEndForPPInFile(file, anchorPP, false, fileLen);
          trace("include/apply",
                "file={0} patch[{1}] INSERT: left-neighbor anchorPP={2} -> "
                "startByte={3}",
                file, idx, anchorPP, startByte);
          if (startByte < 0) {
            continue;
          }
        } else if (decl) {
          // 3) No PP neighbor at all in this header, but we have an owning
          // decl: anchor at the end of its header span.
          startByte = std::clamp(decl->headerE, 0, fileLen);
          trace(
              "include/apply",
              "file={0} patch[{1}] INSERT: no neighbors; anchor at declEnd={2}",
              file, idx, startByte);
        } else {
          // 4) Fallback: use child '#include' sites inside this header as
          // synthetic anchors.
          const int insertByte = ComputeChildBoundaryInsertByte(p, file);
          debug("include/apply.", "inserted byte {0}", insertByte);
          if (insertByte >= 0 && insertByte <= fileLen) {
            std::string text =
                PadAtBoundaries(headerText, static_cast<size_t>(insertByte),
                                static_cast<size_t>(insertByte), p.insertBytes,
                                /* allowLeft */ true, /* allowRight */ true);
            edits.push_back(MakeTextEditWithResyncOrPending(
                headerText, insertByte, insertByte, text, file));

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
      int aLo = std::max(p.aStart, ppLo);
      int aHi = std::min(p.aEnd, ppHi);
      if (aHi <= aLo) {
        // Nothing of this patch lies in this header/declaration.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: empty intersection; SKIP",
              file, idx);
        continue;
      }

      int firstPP = -1, lastPP = -1;
      for (int pp = aLo; pp < aHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          if (firstPP < 0)
            firstPP = pp;
          lastPP = pp;
        }
      }

      if (firstPP < 0 || lastPP < 0) {
        // No tokens from this patch actually map into this header file.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: no mapped PP tokens in "
              "header; SKIP",
              file, idx);
        continue;
      }

      startByte = ByteStartForPPInFile(file, firstPP, /* fallbackToEOF */ false,
                                       fileLen);
      endByte =
          ByteEndForPPInFile(file, lastPP, /* fallbackToEOF */ false, fileLen);
      trace("include/apply",
            "file={0} patch[{1}] DELETE/REPLACE: firstPP={2} lastPP={3} -> "
            "bytes=[{4},{5})",
            file, idx, firstPP, lastPP, startByte, endByte);
      if (startByte < 0 || endByte < 0) {
        continue;
      }
    }

    // Clamp to the declaration’s header_span, if any, so we never cross decl
    // boundaries for DELETE/REPLACE.  For INSERTs we intentionally allow the
    // anchor to sit on the boundary between two declarations so that a new
    // declaration can be injected cleanly between them.
    if (!isInsert && decl) {
      startByte = std::clamp(startByte, decl->headerB, decl->headerE);
      endByte = std::clamp(endByte, decl->headerB, decl->headerE);
    }

    // Sanity clamp to file bounds.
    if (startByte < 0)
      continue;
    startByte = std::clamp(startByte, 0, fileLen);
    endByte = std::clamp(endByte, startByte, fileLen);

    std::string replacement = isDelete ? "" : p.insertBytes;

    if (inTraceMode()) {
      StringRef opKind =
          isInsert ? "INSERT" : (isDelete ? "DELETE" : "REPLACE");

      // DEBUG: log the exact slice and replacement we are about to apply.
      const std::string originalSlice =
          headerText.substr(startByte, endByte - startByte);
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
            formatv("kind={0} name={1} header=[{2},{3}) ppSpan=[{4},{5})",
                    decl->kind, decl->name, decl->headerB, decl->headerE,
                    decl->ppSpan.begin, decl->ppSpan.end)
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
        headerText, startByte, endByte, replacement, file));
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

    if (e.start < 0 || e.end < e.start ||
        e.end > static_cast<int>(headerText.size())) {
      fatal("include/apply", "TextEdit out of bounds: bytes=[{0},{1}) size={2}",
            e.start, e.end, headerText.size());
    }
  }

  return edits;
}

int RefoldEngine::ComputeChildBoundaryInsertByte(const IncludePatch &p,
                                                 StringRef file) const {
  // Which include are we editing?
  const RefoldModel::IncludeItem *owner = p.include;
  if (!owner) {
    return -1;
  }

  // We only care about children whose sitePath is this header file.
  const RefoldModel::IncludeItem *left =
      nullptr; // last child whose coverEnd <= pos
  const RefoldModel::IncludeItem *right =
      nullptr; // first child whose coverBegin >= pos

  int pos = p.aStart; // PP position of the INSERT gap

  for (const auto &child : model_.GetIncludes()) {
    // Only check direct children of the owner
    if (!child.parent || *child.parent != owner->id) {
      continue;
    }

    // Paths must match the file currently being processed
    if (!PathsEqual(child.sitePath, file)) {
      continue;
    }

    int cb = child.cover.begin;
    int ce = child.cover.end;

    // If the INSERT PP-index is *strictly inside* a child's cover, this
    // fallback is the wrong mechanism (that should have been a child-owned
    // patch). Gaps on the boundaries (pos == cb or pos == ce) are valid
    // "between-children" positions and must *not* trigger this guard.
    if (cb < pos && pos < ce) {
      return -1;
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

  return -1;
}

RefoldEngine::ResyncOutcome
RefoldEngine::ApplyResyncOrPend(StringRef originalFileText, int start, int end,
                                StringRef replacement,
                                StringRef fileSpellingForDirective) const {
  int origNl = stringutils::countNewlines(originalFileText, start, end);
  int replNl = stringutils::countNewlines(replacement, 0,
                                          static_cast<int>(replacement.size()));
  if (origNl == replNl)
    return ResyncOutcome(replacement.str(), std::nullopt);

  // Drift detected: decide whether to inject a local `#line` or defer.
  int resumeLine = stringutils::lineAtOffset(originalFileText, end);
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

  DenseMap<std::pair<int, int>, const TextEdit *> bySpan;
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

  int cursor = 0;
  int n = static_cast<int>(originalFileText.size());

  for (const auto *e : norm) {
    if (e->start < 0 || e->end < e->start || e->end > n) {
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
    SmallVectorImpl<char> &out, llvm::StringRef original, int from, int to,
    std::optional<RefoldEngine::PendingResync> pending) const {
  if (!pending || !lineDirs_.Enabled()) {
    auto slice = original.slice(from, to);
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  int i = from;

  // Only flush immediately if BOTH:
  // (1) output is at BOL, and
  // (2) the next original slice begins at BOL in the original file
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      stringutils::isBOL(original, from)) {

    int line = stringutils::lineAtOffset(original, from);
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

    auto slice = original.slice(from, to);
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  // Otherwise, scan forward for the first safe newline boundary (not
  // line-spliced).
  while (i < to) {
    size_t nl = original.find('\n', i);
    if (nl == llvm::StringRef::npos || static_cast<int>(nl) >= to)
      break;

    int nlIdx = static_cast<int>(nl);
    auto slice = original.slice(i, nlIdx + 1);
    out.append(slice.begin(), slice.end());

    // Update our view of the output after appending the newline
    StringRef currentOut(out.data(), out.size());
    i = nlIdx + 1;

    if (!stringutils::isLineSplice(original, nlIdx)) {
      int line = stringutils::lineAtOffset(original, i);
      std::string directive =
          lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

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

  auto remaining = original.slice(i, to);
  out.append(remaining.begin(), remaining.end());
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
  int a0 = (h.aStart >= 0 && static_cast<size_t>(h.aStart) < aTokOff_.size())
               ? aTokOff_[h.aStart]
               : -1;
  int a1 = (h.aEnd >= 0 && static_cast<size_t>(h.aEnd) < aTokOff_.size())
               ? aTokOff_[h.aEnd]
               : -1;

  int b0 = (h.bStart >= 0 && static_cast<size_t>(h.bStart) < bTokOff_.size())
               ? bTokOff_[h.bStart]
               : -1;
  int b1 = (h.bEnd >= 0 && static_cast<size_t>(h.bEnd) < bTokOff_.size())
               ? bTokOff_[h.bEnd]
               : -1;

  StringRef aSlice = "";
  if (a0 >= 0 && a1 >= a0 && static_cast<size_t>(a1) <= aSource_.size()) {
    aSlice = aSource_.substr(a0, a1 - a0);
  }
  StringRef bSlice = "";
  if (b0 >= 0 && b1 >= b0 && static_cast<size_t>(b1) <= bSource_.size()) {
    bSlice = bSource_.substr(b0, b1 - b0);
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

  os << "{kind=" << static_cast<int>(sp.kind) << ", A=[" << sp.begin << ','
     << sp.end << ')' << ", argIdx=" << sp.argIdx;

  if (isStringifyOcc) {
    os << ", occ=STRINGIFY";
  }

  if (sp.kind == RefoldModel::PPArgSpanKind::Paste) {
    os << ", byte=[" << sp.byteBegin << ',' << sp.byteEnd << ')';
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
