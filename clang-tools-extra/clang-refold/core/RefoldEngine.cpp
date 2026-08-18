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
// Strict-Domain Theorem
// ---------------------
// The implementation is organized around the theorem vocabulary declared in the
// focused proof/witness carrier headers.  An emitted edit is either an
// in-domain accepted result with a declared AcceptedProofClass, or an explicit
// terminal out-of-domain result with a named TerminalFallbackProofFailure.  No
// implementation-origin “fallback worked” path is allowed to stand in for a
// theorem-facing proof.
//
// In-domain completeness is restricted to finite deterministic owner-closed
// edit tilings whose state-transition summaries compose and whose preserved
// suffix observers are state-equivalent to B.  All other edits must fail closed
// by materializing a closed owner surface when possible or by emitting the
// named terminal out-of-domain carrier.
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
//     byte-offset drift.
//   • Boundary padding inserts at most one space locally when needed by
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
//   • Paths are compared via the RefoldPathIdentity canonicalization service.
//   • All indices are half-open where applicable: tokens [lo,hi), bytes [b,e).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "core/RefoldEngine.h"

#include "core/RefoldLangOptions.h"
#include "core/RefoldLog.h"
#include "core/RefoldOwnerClassifier.h"
#include "edit/RefoldBInsertionLedger.h"
#include "edit/RefoldExpansionFallbackPlanner.h"
#include "edit/RefoldFinalTUEmissionPlanner.h"
#include "edit/RefoldTUAnchorProof.h"
#include "edit/RefoldTUEditPlanner.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldIncludeInsertionPlanner.h"
#include "include/RefoldIncludeMaterializationScheduler.h"
#include "include/RefoldIncludeMaterializer.h"
#include "include/RefoldPragmaOnceGuardRewriter.h"
#include "include/RefoldIncludeReplayProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldCounterStabilization.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroStateRepairPlanner.h"
#include "proof/RefoldNeutralityProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "sideband/RefoldSidebandPragmaEdits.h"
#include "source/RefoldMixedOwnerTilingPlanner.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldPreprocessingStructureIndexProvider.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "source/RefoldTokenDiffPlanner.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return the macro expansion an A-token position falls strictly inside, or
/// null when the position is an expansion boundary or outside every expansion.
///
/// Boundaries are deliberately not interior: a hunk edge that coincides with a
/// span edge already owns whole expansions on both sides.
/// A hunk edge strictly inside an expansion is not by itself a problem: an edit
/// confined to a macro argument has both edges inside the expansion and is
/// realized by replaying the callsite, which owns the whole expansion.  The
/// unrealizable shape is a *straddle*, where the hunk holds part of an
/// expansion and the neighbouring untouched region holds the rest, so neither
/// can reproduce it.  \p oppositeEdgeInsideSpan distinguishes the two: it is
/// true when the hunk's other edge also lies within the span, meaning the hunk
/// is contained rather than straddling.
const RefoldModel::PPSpan *
macroExpansionStraddledAtEdge(const RefoldModel &model, uint64_t edge,
                              uint64_t oppositeEdge, bool edgeIsLeft) {
  for (const RefoldModel::MacroInvocation &invocation :
       model.GetMacroInvocations()) {
    if (!invocation.invB || !invocation.invE)
      continue;
    for (const RefoldModel::PPSpan &span : invocation.spans) {
      if (!(span.begin < edge && edge < span.end))
        continue;
      // Contained: the whole hunk lies within this expansion, so the callsite
      // realizers own it and the edge is legitimate.
      const bool containedInSpan = edgeIsLeft ? (oppositeEdge <= span.end)
                                              : (span.begin <= oppositeEdge);
      if (containedInSpan)
        continue;
      return &span;
    }
  }
  return nullptr;
}

/// Return whether two A/B tokens are the same lexeme, so retracting a hunk edge
/// across them restores a match rather than inventing one.
bool aAndBTokensAreIdentical(ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
                             uint64_t aToken, uint64_t bToken) {
  if (aToken >= aToks.size() || bToken >= bToks.size())
    return false;
  return aToks[static_cast<size_t>(aToken)].kind ==
             bToks[static_cast<size_t>(bToken)].kind &&
         aToks[static_cast<size_t>(aToken)].spelling ==
             bToks[static_cast<size_t>(bToken)].spelling;
}

/// Verify that every durable structural segment binding names one exact
/// normalized hunk and the matching token edge in its parent witness.
///
/// The tiler lowers proof-rich structural partitions back into ordinary
/// `diffutils::Hunk` values.  Insertion provenance and owner dispatch index
/// those lowered hunks by position, so accepting a stale binding from an
/// intermediate normalization would silently attach the wrong proof carrier.
/// This check is deliberately independent of owner classification: it proves
/// only that the normalization transaction published one coherent hunk/ledger
/// snapshot before downstream planning begins.
bool structuralTilingLedgersMatchNormalizedHunks(
    ArrayRef<diffutils::Hunk> hunks,
    ArrayRef<MixedOwnerTilingWitness> witnesses,
    ArrayRef<MixedOwnerTilingSegmentBinding> bindings, std::string &failure) {
  for (const MixedOwnerTilingSegmentBinding &binding : bindings) {
    if (binding.witnessIndex >= witnesses.size()) {
      failure = llvm::formatv(
                    "binding witness index {0} is outside ledger size {1}",
                    binding.witnessIndex, witnesses.size())
                    .str();
      return false;
    }

    const MixedOwnerTilingWitness &witness =
        witnesses[binding.witnessIndex];
    if (witness.witnessId != binding.parentTilingWitnessId) {
      failure = llvm::formatv(
                    "binding witness id {0} does not match ledger id {1}",
                    binding.parentTilingWitnessId, witness.witnessId)
                    .str();
      return false;
    }
    if (binding.segmentIndex >= witness.edges.size()) {
      failure = llvm::formatv(
                    "binding segment index {0} is outside witness edge count "
                    "{1}",
                    binding.segmentIndex, witness.edges.size())
                    .str();
      return false;
    }

    const MixedOwnerTilingSegmentWitness &edge =
        witness.edges[binding.segmentIndex];
    if (edge.kind != MixedOwnerTilingEdgeKind::TokenSegment ||
        edge.aStart != binding.aStart || edge.aEnd != binding.aEnd ||
        edge.bStart != binding.bStart || edge.bEnd != binding.bEnd) {
      failure = llvm::formatv(
                    "binding A=[{0},{1}) B=[{2},{3}) does not match token "
                    "edge #{4} of witness {5}",
                    binding.aStart, binding.aEnd, binding.bStart, binding.bEnd,
                    binding.segmentIndex, binding.parentTilingWitnessId)
                    .str();
      return false;
    }

    size_t matchingHunkCount = 0;
    for (const diffutils::Hunk &hunk : hunks) {
      if (hunk.aStart == binding.aStart && hunk.aEnd == binding.aEnd &&
          hunk.bStart == binding.bStart && hunk.bEnd == binding.bEnd) {
        ++matchingHunkCount;
      }
    }
    if (matchingHunkCount != 1) {
      failure = llvm::formatv(
                    "binding A=[{0},{1}) B=[{2},{3}) matches {4} normalized "
                    "hunks instead of exactly one",
                    binding.aStart, binding.aEnd, binding.bStart, binding.bEnd,
                    matchingHunkCount)
                    .str();
      return false;
    }
  }

  // Every emitted token edge must also have exactly one reverse binding.  The
  // forward check above rejects stale bindings; this reverse check rejects a
  // partially published witness whose segment would reach owner dispatch
  // without the structural authority that justified its split.
  for (size_t witnessIndex = 0; witnessIndex < witnesses.size();
       ++witnessIndex) {
    const MixedOwnerTilingWitness &witness = witnesses[witnessIndex];
    for (size_t edgeIndex = 0; edgeIndex < witness.edges.size(); ++edgeIndex) {
      const MixedOwnerTilingSegmentWitness &edge = witness.edges[edgeIndex];
      if (edge.kind != MixedOwnerTilingEdgeKind::TokenSegment)
        continue;

      size_t matchingBindingCount = 0;
      for (const MixedOwnerTilingSegmentBinding &binding : bindings) {
        if (binding.witnessIndex == witnessIndex &&
            binding.parentTilingWitnessId == witness.witnessId &&
            binding.segmentIndex == edgeIndex) {
          ++matchingBindingCount;
        }
      }
      if (matchingBindingCount != 1) {
        failure = llvm::formatv(
                      "token edge #{0} of witness {1} has {2} reverse "
                      "bindings instead of exactly one",
                      edgeIndex, witness.witnessId, matchingBindingCount)
                      .str();
        return false;
      }
    }
  }

  return true;
}

} // namespace

RefoldEngine::RefoldEngine(
    RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict,
    ProofAuditMode proofAuditMode, StringRef finalOutputPath,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    FinalLineControlValidationCallback finalLineControlValidationCallback,
    std::optional<AlignmentSelectionOverride> alignmentSelectionOverride,
    bool alignmentSemanticResolverEnabled,
    std::optional<StringRef> tuSourceBytesOverride)
    : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
      aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff),
      noLines_(noLines), finalOutputPath_(finalOutputPath.str()),
      lineDirs_(!noLines, model_.GetPPCwd()),
      pathIdentity_(model_, model_.GetPPCwd(), /*emitAbsPaths=*/false),
      strict_(strict), proofAuditMode_(proofAuditMode),
      lexLang_(makeRefoldLexLangOptions(model_.GetPPLang())),
      argTextRecovery_(lexLang_), tokenTextAnalysis_(lexLang_),
      terminalSink_(RefoldTerminalProofSinkCallbacks{
          [this](const TerminalFallbackProofFailure &failure, StringRef role) {
            TheoremAudit().AuditTerminalFallbackForLegacyAuthority(failure,
                                                                   role);
          },
          [this](StringRef detail) {
            TheoremAudit().NoteTheoremAuditViolation(detail);
          },
          [this](const TerminalFallbackRequest &request) {
            ProofLattice().WitnessTrace().TraceWitnessFallback(request);
          }}),
      finalReplaySurface_(buildFinalReplaySurface(model_, finalOutputPath)),
      materializedEditMappings_(materializedEditMappings),
      finalLineControlValidationCallback_(
          std::move(finalLineControlValidationCallback)),
      sidebandPragmaEdits_(sidebandPragmaEdits.begin(),
                           sidebandPragmaEdits.end()),
      sourceMapper_(model_, pathIdentity_, aSource_, bSource_, aToks_, bToks_,
                    aTokOff_, bTokOff_, abTokHunks_, abByteHunks_,
                    abByteHunkPrefixDelta_, strict_),
      macroTopology_(model_, aToks_, bToks_, sourceMapper_, pathIdentity_),
      macroBoundarySelector_(model_, macroTopology_, sourceMapper_, bToks_,
                             lexLang_),
      lineControlProof_(model_, pathIdentity_, macroTopology_, lineDirs_,
                        aToks_, bToks_, abTokMapA2B_, abTokMapB2A_) {
  alignmentSelectionOverride_ = std::move(alignmentSelectionOverride);
  alignmentSemanticResolverEnabled_ = alignmentSemanticResolverEnabled;
  alignmentSemanticTheoremActive_ = alignmentSelectionOverride_.has_value();
  tuSourceBytesOverride_ = std::move(tuSourceBytesOverride);

  // Build the object graph in dependency order.  Each service receives
  // explicit inputs/services and, where orchestration remains engine-owned, a
  // narrow hook bundle; no service stores RefoldEngine itself.
  InitializeTheoremAudit();
  InitializeOwnerStateProof();
  InitializeMacroStateProof();
  InitializePreprocessingStructureIndex();
  InitializePreprocessingStructureIndexProvider();
  InitializeTokenDiffPlanner();
  InitializeTUAnchorProof();
  InitializeTUEditPlanner();
  InitializeOwnerClassifier();
  InitializeMixedOwnerTilingPlanner();
  InitializeBInsertionLedger();
  InitializeCounterStabilization();
  InitializeProofLattice();
  InitializeMacroPatchPlanner();
  InitializeIncludeInsertionPlanner();
  InitializeTextEditAssembler();
  InitializeMacroStateRepairPlanner();
  InitializeLineObserverLayout();
  InitializePragmaOnceGuardRewriter();
  InitializeIncludeMaterializer();
  InitializeExpansionFallbackPlanner();
}

RefoldEngine::~RefoldEngine() = default;

// ========================== Public entry points ==========================

/// Attach the edited-stream token an assembly check disagreed at.
///
/// The failed obligation is about preprocessor state, so the state component
/// stays exactly as the proof recorded it.  What the check additionally knows,
/// and previously discarded, is *where* the two streams parted: a single edited
/// token index.  That is the difference between "this translation unit is out
/// of domain" and "the edit geometry around this token is", and the retry ladder
/// needs it to tell a failure a different alignment could repair from one it
/// could not.
static TerminalFallbackProofFailure
withDivergingEditedToken(TerminalFallbackProofFailure failure,
                         std::size_t mismatchTokenIndex) {
  failure.context.bTokenBegin = static_cast<uint64_t>(mismatchTokenIndex);
  failure.context.bTokenEnd = static_cast<uint64_t>(mismatchTokenIndex) + 1;
  return failure;
}

/// Describe which attribution surfaces one terminal request carries.
///
/// A region-scoped realization needs somewhere to put the payload it cannot
/// prove, and the only fields the narrowing ladder reads are `ownerId` and the A
/// token range. Reporting the whole surface -- including the hunk, the source
/// span, and the B token range -- is what turns "does every request name a
/// region" into a question with an observable answer rather than an audit.
static std::string describeTerminalRequestAttribution(
    const TerminalFallbackFailureContext &context) {
  std::string text;
  llvm::raw_string_ostream os(text);
  auto field = [&os](StringRef name, const std::optional<uint64_t> &value) {
    os << ' ' << name << '=';
    if (value)
      os << *value;
    else
      os << '-';
  };

  field("ownerId", context.ownerId);
  field("hunk", context.hunk);
  os << " source=";
  if (context.sourcePath && context.sourceBegin && context.sourceEnd)
    os << *context.sourcePath << '[' << *context.sourceBegin << ','
       << *context.sourceEnd << ')';
  else
    os << '-';
  os << " aTokens=";
  if (context.aTokenBegin && context.aTokenEnd)
    os << '[' << *context.aTokenBegin << ',' << *context.aTokenEnd << ')';
  else
    os << '-';
  os << " bTokens=";
  if (context.bTokenBegin && context.bTokenEnd)
    os << '[' << *context.bTokenBegin << ',' << *context.bTokenEnd << ')';
  else
    os << '-';
  return text;
}

/// Return whether a terminal request names any region at all.
///
/// This is the precondition a region-scoped realization needs from every
/// request. It is deliberately weaker than what such a realization will
/// ultimately require -- a named region must additionally be placeable between
/// preserved directives -- so a run reporting every request attributed is
/// necessary, not sufficient.
static bool terminalRequestNamesRegion(
    const TerminalFallbackFailureContext &context) {
  return context.ownerId.has_value() || context.hunk.has_value() ||
         (context.sourceBegin.has_value() && context.sourceEnd.has_value()) ||
         (context.aTokenBegin.has_value() && context.aTokenEnd.has_value());
}

/// Take the raw-B terminal carrier as this run's final answer.
///
/// This is the one seam where the driver gives up the entire translation unit:
/// either the narrowing ladder is spent and every region a terminal request
/// named is already expanded, or the closing assembly check could not attribute
/// its divergence to any region that is not. Both callers have already produced
/// the carrier text; routing them through here makes "this run emitted raw B" a
/// single observable fact instead of two returns that must be kept in step.
///
/// It is deliberately not the per-attempt summary in
/// `emitRefoldAttemptStatsSummary()`. That runs once per attempt *and* once per
/// candidate simulation, so it reports terminal requests the ladder went on to
/// repair: a run that recovers still prints `terminalFallback=yes(raw-B)` there,
/// and a run that surrenders can print `terminalFallback=no`. Only the driver
/// frame knows which attempt was the last one, and candidate simulations never
/// reach it -- they call the instance `Refold()` directly.
///
/// This is also where whole-file surrender is meant to be replaced by a
/// region-scoped realization, which is why the attribution census below reports
/// what each request left behind to work from.
/// One boundary-straddling deletion run moved onto its owner's cover.
struct OwnerAlignedDeletionSlide {
  std::vector<int64_t> map;
  /// The run before and after the move, and the signed token distance.
  uint64_t originalABegin = 0;
  uint64_t originalAEnd = 0;
  uint64_t repairedABegin = 0;
  uint64_t repairedAEnd = 0;
  int64_t offset = 0;
  /// A tokens whose anchor the move created; they are no longer core-forced.
  SmallVector<uint64_t, 4> movedAnchors;
};

/// Return the innermost include owning one A token, or nullopt for the TU.
static std::optional<uint64_t> innermostOwnerForAToken(const RefoldModel &model,
                                                       uint64_t aToken) {
  return model.InnermostIncludeAtPP(aToken);
}

/// Return whether every A token of `[aBegin, aEnd)` has the same owner.
///
/// This is the property that lets a single owner realize the run. A run failing
/// it is exactly the shape whose `OwnerClosedCover` obligation cannot be
/// discharged, because no owner covers all of its tokens.
static bool runHasOneOwner(const RefoldModel &model, uint64_t aBegin,
                           uint64_t aEnd) {
  if (aEnd <= aBegin)
    return false;
  const std::optional<uint64_t> owner =
      innermostOwnerForAToken(model, aBegin);
  for (uint64_t aToken = aBegin + 1; aToken < aEnd; ++aToken) {
    if (innermostOwnerForAToken(model, aToken) != owner)
      return false;
  }
  return true;
}

/// Return whether `[aBegin, aEnd)` is a maximal unmapped run of `map`.
static bool isMaximalDeletionRun(ArrayRef<int64_t> map, uint64_t aBegin,
                                 uint64_t aEnd) {
  if (aEnd <= aBegin || aEnd > map.size())
    return false;
  for (uint64_t aToken = aBegin; aToken < aEnd; ++aToken) {
    if (map[aToken] >= 0)
      return false;
  }
  if (aBegin > 0 && map[aBegin - 1] < 0)
    return false;
  if (aEnd < map.size() && map[aEnd] < 0)
    return false;
  return true;
}

/// Move a straddling deletion run onto a single owner, when the tokens allow.
///
/// A deletion run `[s, e)` may slide one position left when `A[s-1]` and
/// `A[e-1]` have the same spelling, and one position right when `A[s]` and
/// `A[e]` do. Either move keeps the map strictly monotone and preserves the
/// exact matched-token count, so the result is still a maximum-length common
/// subsequence -- it is a different optimal alignment, not a worse one.
///
/// It is not optimal under the owner-depth tie-break, and that is the whole
/// point: that tie-break is an additive per-deleted-token cost, so it rewards a
/// run for swallowing a shallow token in place of a deep one. At an include
/// boundary that reward is exactly what pulls a run off its owner's cover and
/// onto a straddle no owner can realize. Correcting the tie-break globally is
/// not sound -- a run that crosses a boundary while *fully consuming* the
/// include on the far side realizes perfectly well, and penalizing it relocates
/// includes across conditional boundaries. So the correction is applied here,
/// scoped to a run whose proof has already failed.
///
/// Returns nullopt when no request names a straddling run, or when no admissible
/// slide makes one owner-uniform. The caller then proceeds to the existing
/// narrowing ladder unchanged.
static std::optional<OwnerAlignedDeletionSlide>
buildOwnerAlignedDeletionSlide(const RefoldModel &model, ArrayRef<PPTok> aToks,
                               ArrayRef<int64_t> baseMap,
                               ArrayRef<TerminalFallbackRequest> requests) {
  // A slide is only ever a small correction: the run is being nudged back onto
  // a cover it already overlaps, not searched for across the stream.
  constexpr uint64_t maxSlideDistance = 64;

  auto spellingsEqual = [&](uint64_t lhs, uint64_t rhs) {
    return lhs < aToks.size() && rhs < aToks.size() &&
           aToks[lhs].spelling == aToks[rhs].spelling;
  };

  for (const TerminalFallbackRequest &request : requests) {
    const TerminalFallbackFailureContext &context = request.failure.context;
    if (!context.aTokenBegin || !context.aTokenEnd || !context.bTokenBegin ||
        !context.bTokenEnd)
      continue;
    // Only a pure deletion slides: a replacement's B payload is anchored to the
    // A interval it replaces, so moving the interval would change what the
    // payload realizes.
    if (*context.bTokenBegin != *context.bTokenEnd)
      continue;

    const uint64_t aBegin = *context.aTokenBegin;
    const uint64_t aEnd = *context.aTokenEnd;
    if (!isMaximalDeletionRun(baseMap, aBegin, aEnd))
      continue;
    // A run already covered by one owner is not this defect.
    if (runHasOneOwner(model, aBegin, aEnd))
      continue;

    const uint64_t runLength = aEnd - aBegin;
    for (uint64_t distance = 1; distance <= maxSlideDistance; ++distance) {
      // Left first, then right, so the choice does not depend on iteration
      // order anywhere else.
      for (int direction : {-1, 1}) {
        const bool left = direction < 0;
        if (left && aBegin < distance)
          continue;
        if (!left && aEnd + distance > baseMap.size())
          continue;

        const uint64_t movedBegin =
            left ? aBegin - distance : aBegin + distance;
        const uint64_t movedEnd = movedBegin + runLength;

        // Every step of the slide must exchange two identically spelled
        // tokens, or the map would stop agreeing with the streams.
        bool admissible = true;
        for (uint64_t step = 0; step < distance && admissible; ++step) {
          admissible = left ? spellingsEqual(aBegin - 1 - step,
                                             aEnd - 1 - step)
                            : spellingsEqual(aBegin + step, aEnd + step);
        }
        if (!admissible)
          continue;
        if (!runHasOneOwner(model, movedBegin, movedEnd))
          continue;

        OwnerAlignedDeletionSlide slide;
        slide.map.assign(baseMap.begin(), baseMap.end());
        slide.originalABegin = aBegin;
        slide.originalAEnd = aEnd;
        slide.repairedABegin = movedBegin;
        slide.repairedAEnd = movedEnd;
        slide.offset = left ? -static_cast<int64_t>(distance)
                            : static_cast<int64_t>(distance);
        for (uint64_t step = 0; step < distance; ++step) {
          const uint64_t vacated = left ? aBegin - 1 - step : aEnd + step;
          const uint64_t claimed = left ? aEnd - 1 - step : aBegin + step;
          slide.map[claimed] = slide.map[vacated];
          slide.map[vacated] = -1;
          slide.movedAnchors.push_back(claimed);
        }
        return slide;
      }
    }
  }
  return std::nullopt;
}

/// Build the alignment override that realizes one owner-alignment repair.
///
/// Anchors the core theorem forced keep that proof. The anchors the slide
/// created carry `OwnerAlignedDeletionSlide` instead, because they are not on
/// every core-optimal path -- the map is match-count optimal but deliberately
/// not depth-optimal.
static AlignmentSelectionOverride buildOwnerAlignedSlideOverride(
    const OwnerAlignedDeletionSlide &slide,
    const diffutils::CertifiedLcsResult &coreAlignment) {
  AlignmentSelectionOverride selection;
  selection.selectedMap = slide.map;
  selection.selectedAnchorProofs.assign(slide.map.size(),
                                        diffutils::LcsAnchorProof{});
  selection.globalObjective = coreAlignment.globalObjective;
  selection.globalObjectiveIsExact = coreAlignment.globalObjectiveIsExact;
  selection.allWindowsCertified = coreAlignment.allWindowsCertified;
  selection.certifiedBoundaries = coreAlignment.certifiedBoundaries;
  selection.certificationWindows = coreAlignment.certificationWindows;

  for (size_t aToken = 0; aToken < slide.map.size(); ++aToken) {
    if (slide.map[aToken] < 0)
      continue;
    if (llvm::is_contained(slide.movedAnchors, aToken)) {
      selection.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
          diffutils::LcsAnchorProofKind::OwnerAlignedDeletionSlide, 0};
      continue;
    }
    if (aToken < coreAlignment.selectedAnchorProofs.size()) {
      selection.selectedAnchorProofs[aToken] =
          coreAlignment.selectedAnchorProofs[aToken];
      continue;
    }
    selection.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
        diffutils::LcsAnchorProofKind::OwnerAlignedDeletionSlide, 0};
  }
  return selection;
}

static llvm::Error
takeTerminalCarrier(std::string carrier,
                    ArrayRef<TerminalFallbackRequest> requests) {
  uint64_t attributed = 0;
  for (const TerminalFallbackRequest &request : requests) {
    const bool namesRegion = terminalRequestNamesRegion(request.failure.context);
    attributed += namesRegion ? 1 : 0;
    REFOLD_LOG_WARN("fallback/carrier",
                    "  request obligation={0} reason={1} stage={2} "
                    "namesRegion={3}{4}",
                    request.failure.obligation, request.failure.reason,
                    request.stage.empty() ? StringRef("<unspecified>")
                                          : StringRef(request.stage),
                    namesRegion ? "yes" : "no",
                    describeTerminalRequestAttribution(request.failure.context));
  }

  REFOLD_LOG_WARN("fallback/carrier",
                  "emitting the raw edited preprocessed stream for the whole "
                  "translation unit ({0} bytes): requests={1} attributed={2} "
                  "unattributed={3}",
                  static_cast<uint64_t>(carrier.size()),
                  static_cast<uint64_t>(requests.size()), attributed,
                  static_cast<uint64_t>(requests.size()) - attributed);

  // Fail rather than emit the edited preprocessed stream.
  //
  // Surrendering looks safe to the closing check -- the carrier *is* B, so it
  // replays B exactly -- while deleting every comment and directive in the
  // file. That is the one outcome the refolder may never produce, so the
  // absence of a proof is reported as the absence of an answer.
  //
  // Reaching here means no admissible realization was found for some region and
  // no ladder rung could repair it. That can be correct: a payload whose side
  // of a preserved directive is genuinely undetermined has no sound placement,
  // because a pragma may change compiled meaning without changing the
  // preprocessed tokens, so both placements pass the closing check while
  // differing in what they mean. The right answer there is to say so, not to
  // silently drop the directive along with the rest of the file's structure.
  //
  // The census above names every request that led here, so the failure carries
  // its own attribution.
  (void)carrier;
  return createStringError(
      std::make_error_code(std::errc::illegal_byte_sequence),
      "no admissible refold: %llu terminal request(s) reached the seam and "
      "none could be narrowed; emitting the edited preprocessed stream would "
      "drop every comment and directive in the translation unit. See the "
      "fallback/carrier census above for the failing obligations",
      static_cast<unsigned long long>(requests.size()));
}

Expected<std::string> RefoldEngine::Refold(
    const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict,
    ProofAuditMode proofAuditMode, StringRef finalOutputPath,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    std::vector<MaterializedEditMapping> *materializedEditMappings,
    FinalLineControlValidationCallback finalLineControlValidationCallback,
    OutputVerificationMode verifyMode,
    ArrayRef<std::string> verifyIncludeDirs) {
  // Build the refold model based on the parsed JSON object.
  auto mOrErr = RefoldModel::FromJson(rootJson);
  if (!mOrErr)
    return mOrErr.takeError();

  // Build the closing assembly check before the engine runs.  Everything it
  // needs is already here: the producer context, the edited stream, and this
  // run's relaxation mode.  A verifier that cannot be built simply leaves the
  // check absent.
  //
  // `Off` builds nothing at all: constructing the verifier preprocesses the
  // edited stream, so a caller that does not want the check should not pay for
  // it.
  //
  // The assembly is re-preprocessed beside the *producer's* source, never
  // beside the refold output: a quoted include resolves against the including
  // file's own directory first, so an output directory holding a later copy of
  // the same header would answer the check against headers the producer never
  // read.  Without that anchor the check is left absent rather than answered
  // wrongly.
  std::optional<RefoldFinalAssemblyVerifier> assemblyVerifier;
  if (verifyMode != OutputVerificationMode::Off) {
    if (auto ctxOrErr = RefoldModel::ParsePreprocessContext(rootJson)) {
      if (auto sourceOrErr = RefoldModel::ParseSourcePath(rootJson)) {
        if (std::optional<std::string> anchor =
                producerSourceAnchorPath(*sourceOrErr, *ctxOrErr))
          assemblyVerifier = RefoldFinalAssemblyVerifier::Create(
              rootJson, *ctxOrErr, bSource, noLines, strict, *anchor,
              verifyIncludeDirs);
      } else {
        consumeError(sourceOrErr.takeError());
      }
    } else {
      consumeError(ctxOrErr.takeError());
    }
  }

  // Narrowing loop.  Each attempt gets a *fresh* engine rather than re-running
  // the pass in place: planning services are built once per engine and several
  // are deliberately single-shot -- the pragma-once guard catalog records its
  // active header set exactly once, for instance -- so a second pass through
  // the same engine is not a supported operation.  Rebuilding is also what the
  // alignment resolver already does for its candidate probes, so the model
  // carries a read-only clone for exactly this purpose.
  //
  // The set of owners ruled out from keeping their callsite is carried across
  // attempts and only grows, which is what makes this terminate: every round
  // either verifies or gives up one more owner, ending at the translation unit.
  llvm::DenseSet<uint64_t> ownersMustExpand;

  // Test-only seed for the narrowing set, with no default effect.
  //
  // Reaching an include through the owner search needs an assembly that
  // diverges at a token no macro invocation covers, which means a live defect.
  // This hook instead states the conclusion the search would have reached, so a
  // test can exercise what marking an owner *does*: the include is expanded by
  // the ordinary materialization path and nothing else in the file moves.  The
  // value is an include target spelling, matched as a substring, because
  // producer ids shift as a map is regenerated while `"leaf.h"` does not.
  if (const char *forcedTarget =
          std::getenv("CLANG_REFOLD_TEST_ONLY_FORCE_EXPAND_INCLUDE")) {
    const StringRef wanted(forcedTarget);
    for (const RefoldModel::IncludeItem &include : (*mOrErr).GetIncludes()) {
      if (!include.target.contains(wanted))
        continue;
      REFOLD_LOG_INFO("assembly-verify",
                      "test-only: marking include {0} (target={1}) must-expand",
                      include.id, include.target);
      ownersMustExpand.insert(include.id);
    }
  }

  // Every diverging region is given up in one attempt, so a further attempt is
  // only ever needed to *widen*: a region that was expanded and still diverges
  // escalates to the region enclosing it.  That is bounded by nesting depth,
  // not by how many regions diverged.
  //
  // There is deliberately no ceiling on the ladder.  A constant bound is not a
  // proof, and it can itself force the whole-translation-unit carrier: a unit
  // whose ladder was still naming fresh regions would stop mid-descent and emit
  // the edited stream for the entire file with narrowing still available.
  // Termination comes from the set instead -- `ownersMustExpand` only grows,
  // every retry must add at least one region that is not already in it, and the
  // number of producer regions is finite -- so the loop below stops when a round
  // makes no progress rather than when a counter runs out.
  //
  // The ladder is still counted separately from the loop, because a
  // resolve-and-re-plan attempt must not be mistaken for a narrowing step: the
  // count is what the closing diagnostics report, and folding resolution into it
  // would misreport how far the ladder actually descended.
  unsigned narrowingAttempts = 0;

  // Alignment ambiguity is resolved on demand.  Every candidate map a window
  // enumerates is realized by a complete refold of the translation unit, so the
  // first attempt plans on the core theorem's forced anchors alone and resolves
  // nothing; only an attempt that produces evidence ambiguity is what limited
  // it turns this on, and then the next attempt resolves every window that can
  // carry ambiguity.
  //
  // It is deliberately one flag rather than a set of windows.  A committed
  // window contributes its anchors to the base map the next window is compared
  // against, so resolving windows {1,3} is not a less complete version of
  // resolving {1,2,3} -- it is a different alignment.  All-or-nothing keeps the
  // resolved attempt byte-identical to what whole-stream resolution produced.
  //
  // Nothing here weighs cost.  The flag is set by evidence and cleared never,
  // so no input is declined into the terminal carrier for being expensive.
  bool resolveAlignmentAmbiguity = false;

  // Resolution is asked once per run and replayed by every attempt after that.
  // See `AlignmentSemanticResolutionMemo`: the question every attempt puts to
  // the resolver is the same one, and answering it costs a complete refold of
  // the translation unit per enumerated candidate map.
  AlignmentSemanticResolutionMemo alignmentResolutionMemo;

  // The core alignment is likewise certified once per run.  See
  // `AlignmentCertificationMemo`: every attempt certifies the identical lexeme
  // streams under the identical budget, and that theorem is the quadratic
  // dynamic program window partitioning exists to bound.
  AlignmentCertificationMemo alignmentCertificationMemo;

  // Set once, by the owner-alignment repair below, and then carried by every
  // later attempt: the repaired alignment is the one the run planned from from
  // that point on.
  std::optional<AlignmentSelectionOverride> ownerAlignedSlideOverride;
  bool attemptedOwnerAlignedSlide = false;

  for (unsigned attempt = 0;; ++attempt) {
    // Each attempt needs its own model.  Moving the parsed one in would leave
    // every later attempt building an engine from a moved-from model -- no
    // includes, no macro invocations, no directives -- so a marked region would
    // have nothing to be marked against and the ladder would appear to make no
    // progress.  `CloneForReadOnlyConsumer` exists for this: a plain copy would
    // leave the clone's lookup tables pointing into the original's storage.
    RefoldEngine engine(
        mOrErr->CloneForReadOnlyConsumer(), aSource, aToks, aTokOff, bSource,
        bToks, bTokOff,
        noLines, strict, proofAuditMode, finalOutputPath, sidebandPragmaEdits,
        materializedEditMappings,
        // Copied, not moved: this runs once per attempt, and a moved-from
        // callback would silently disable final line-control validation for
        // every attempt after the first.
        finalLineControlValidationCallback, ownerAlignedSlideOverride);
    engine.finalAssemblyVerifier_ = assemblyVerifier;
    engine.ownersMustExpand_ = ownersMustExpand;
    engine.resolveAlignmentAmbiguity_ = resolveAlignmentAmbiguity;
    engine.alignmentResolutionMemo_ = &alignmentResolutionMemo;
    engine.alignmentCertificationMemo_ = &alignmentCertificationMemo;
    engine.verifyIncludeDirs_.assign(verifyIncludeDirs.begin(),
                                     verifyIncludeDirs.end());

    std::string out = engine.Refold();

    // Turn resolution on and re-plan when this attempt showed that alignment
    // ambiguity is what limited it.  This happens at most once per run: the
    // resolved attempt already resolves every window that can carry ambiguity,
    // so a further request would ask the identical question of the identical
    // alignment.
    //
    // It runs before the narrowing ladders below because resolving ambiguity
    // gives up nothing, while expanding a region trades away a preserved macro
    // or include permanently.  The ladders keep their full allowance: `attempt`
    // is not what bounds them.
    if (!resolveAlignmentAmbiguity && engine.AlignmentResolutionIsDemanded()) {
      // The evidence asks for resolution, so resolve -- but only a resolution
      // that commits a window changes what the next attempt would plan.  When
      // every ambiguous window keeps its core-forced anchors,
      // `ResolveSemanticAlignment()` retains exactly the map this attempt
      // planned from, so the re-planned attempt would re-derive this attempt's
      // output byte for byte.
      //
      // So publish the theorem first and re-plan only when an anchor actually
      // moved.  The probe is not additional work: it is the resolution the next
      // attempt would have run, recorded so that attempt -- and every narrowing
      // attempt after it -- replays the answer instead of realizing every
      // candidate map again.
      resolveAlignmentAmbiguity = true;

      // Resolution moves an anchor only inside a window that carries ambiguity.
      // When the core theorem forced every A token of every certified window,
      // resolution passes over all of them and commits nothing, so the map it
      // publishes is the one this attempt already planned from.  That is
      // knowable from the retained forced map in one linear scan, and it is the
      // common shape: a unit can demand resolution because it conceded a macro
      // root while carrying no ambiguity for resolution to spend itself on.
      if (!engine.AnyCertificationWindowCarriesAmbiguity()) {
        // Falls through to this attempt's terminal and verification handling:
        // the result stands, so it must still be judged like any other.
        REFOLD_LOG_INFO(
            "fallback",
            "attempt {0} is limited by alignment ambiguity, but the core "
            "theorem determined every certified window; resolution has nothing "
            "to commit and this result stands",
            attempt);
      } else {
        // The probe must present the same optional output surfaces as the
        // attempt it stands in for, because a candidate simulation mirrors its
        // parent's surfaces and two alignments may otherwise be separated by
        // provenance production was never asked for.  Only whether the sidecar
        // exists is observable, so scratch storage answers it without
        // disturbing the mappings that belong to the attempt that will be
        // emitted.
        std::vector<MaterializedEditMapping> probeMaterializedEditMappings;
        RefoldEngine probe(
            mOrErr->CloneForReadOnlyConsumer(), aSource, aToks, aTokOff,
            bSource, bToks, bTokOff, noLines, strict, proofAuditMode,
            finalOutputPath, sidebandPragmaEdits,
            materializedEditMappings ? &probeMaterializedEditMappings : nullptr,
            // A probe stops before final line-control pruning, the callback's
            // only consumer, and a candidate simulation is handed an empty one
            // regardless -- so resolution cannot observe its absence.
            FinalLineControlValidationCallback());
        probe.ownersMustExpand_ = ownersMustExpand;
        probe.resolveAlignmentAmbiguity_ = true;
        probe.alignmentResolutionMemo_ = &alignmentResolutionMemo;
        // The probe certifies nothing new: this attempt already recorded the
        // alignment, so the probe replays it and spends its time only on the
        // resolution the next attempt would otherwise have paid for.
        probe.alignmentCertificationMemo_ = &alignmentCertificationMemo;
        probe.verifyIncludeDirs_.assign(verifyIncludeDirs.begin(),
                                        verifyIncludeDirs.end());
        probe.ProbeAlignmentResolution();

        if (alignmentResolutionMemo.resolution.committedEquivalentClass) {
          REFOLD_LOG_INFO(
              "fallback",
              "attempt {0} is limited by alignment ambiguity; resolution "
              "committed {1} window witness(es), re-planning against them",
              attempt,
              static_cast<uint64_t>(
                  alignmentResolutionMemo.resolution.witnesses.size()));
          continue;
        }

        if (!alignmentResolutionMemo.recorded) {
          // The probe stopped before token-diff planning published a theorem,
          // so nothing was proved either way.  A re-planned attempt would reach
          // the same stopping point, so this attempt's result stands.
          REFOLD_LOG_WARN("fallback",
                          "attempt {0} is limited by alignment ambiguity, but "
                          "the resolution probe published no theorem; this "
                          "result stands",
                          attempt);
        } else {
          REFOLD_LOG_INFO("fallback",
                          "attempt {0} is limited by alignment ambiguity, but "
                          "every ambiguous certification window kept its "
                          "core-forced anchors; re-planning would reproduce "
                          "this attempt, so this result stands",
                          attempt);
        }
      }
    }

    // A terminal request means the run gave up and `out` is the edited stream
    // for the whole file.  Before accepting that, see whether any request named
    // the region whose proof failed: ruling that one region out and assembling
    // again is strictly less than giving up the file, and it costs nothing when
    // no request names a region -- which is every run that never fell back.
    //
    // This is deliberately not gated on the verification mode.  It repairs the
    // fallback ladder, not a verification verdict, and a caller that asked for
    // no verification still wants the smaller answer.
    if (engine.terminalSink_.HasRequest()) {
      // Before giving any region up, see whether a request failed only because
      // its deletion run sits across an owner boundary rather than on one.
      //
      // This runs ahead of the narrowing ladder for the same reason resolution
      // does: moving a run costs nothing -- the alignment keeps its exact
      // matched-token count -- while expanding a region trades away a preserved
      // include or macro permanently. It is attempted once per run, because a
      // second attempt would put the identical question to the identical
      // alignment.
      if (!attemptedOwnerAlignedSlide &&
          alignmentCertificationMemo.recorded) {
        attemptedOwnerAlignedSlide = true;
        if (std::optional<OwnerAlignedDeletionSlide> slide =
                buildOwnerAlignedDeletionSlide(
                    *mOrErr, aToks,
                    alignmentCertificationMemo.facts.alignment.selectedMap,
                    engine.terminalSink_.Requests())) {
          REFOLD_LOG_INFO(
              "fallback",
              "terminal fallback names a deletion run A=[{0},{1}) that "
              "straddles an owner boundary; moving it by {2} token(s) to "
              "A=[{3},{4}), where one owner covers it, and re-planning",
              slide->originalABegin, slide->originalAEnd, slide->offset,
              slide->repairedABegin, slide->repairedAEnd);
          ownerAlignedSlideOverride = buildOwnerAlignedSlideOverride(
              *slide, alignmentCertificationMemo.facts.alignment);
          continue;
        }

        // A suppressed anchor widens a hunk into a replacement whose payload is
        // spelled by A tokens at its own edges. Re-anchoring them narrows it
        // back to a deletion, and the candidate narrowings differ in whether
        // the surviving deletion renumbers the lines after it.
        if (std::optional<AlignmentSelectionOverride> narrowing =
                engine.BuildLineAlignedHunkNarrowing(
                    alignmentCertificationMemo.facts.alignment)) {
          ownerAlignedSlideOverride = std::move(narrowing);
          continue;
        }
      }

      // Give up every region that any request names, and take the carrier only
      // when *no* request names one.
      //
      // This deliberately no longer waits for every request to be narrowable.
      // That rule was a cost policy -- it avoided re-assemblies that might not
      // reach a smaller answer -- but the two outcomes it chooses between are
      // not a whole answer and a slightly smaller one: they are a partial
      // refold and a verbatim copy of the entire file.  One unattributable
      // request among fifty would surrender every include, macro and directive
      // in the unit alongside it.  A request naming nothing still contributes
      // nothing here; it simply no longer vetoes the regions that others named.
      llvm::SmallVector<uint64_t, 8> owners;
      const bool everyRequestNarrowable =
          engine.AppendNarrowableOwnersForTerminalRequests(ownersMustExpand,
                                                          owners);
      if (!everyRequestNarrowable)
        REFOLD_LOG_INFO("fallback",
                        "some terminal requests name no region; narrowing the "
                        "{0} region(s) that were named and re-planning",
                        static_cast<uint64_t>(owners.size()));
      if (!owners.empty()) {
        // Progress, not a counter, is what bounds this.  Record only regions
        // the set did not already hold: a round that names nothing new would
        // re-plan the identical input and reach the identical verdict, so it
        // must fall through to the carrier rather than loop.
        bool expandedNewOwner = false;
        for (uint64_t owner : owners) {
          if (!ownersMustExpand.insert(owner).second)
            continue;
          expandedNewOwner = true;
          REFOLD_LOG_INFO("fallback",
                          "terminal fallback names {0}; expanding it and "
                          "retrying instead of the whole translation unit",
                          engine.DescribeOwner(owner));
        }
        if (expandedNewOwner) {
          ++narrowingAttempts;
          continue;
        }
        REFOLD_LOG_WARN("fallback",
                        "terminal fallback named only regions already given "
                        "up after {0} narrowing step(s); taking the carrier",
                        narrowingAttempts);
      }
      // `out` is already the carrier: this attempt's own `Refold()` replaced it
      // when it saw the request.  Nothing narrower is left to give up.
      return takeTerminalCarrier(std::move(out), engine.terminalSink_.Requests());
    }

    // Without a verifier the result stands exactly as it would without this
    // loop.
    if (!assemblyVerifier)
      return out;

    const FinalAssemblyVerdict verdict = assemblyVerifier->Verify(out);
    if (verdict.verified || verdict.inconclusive) {
      if (verdict.inconclusive)
        // An inconclusive verdict is not a rejection, but it does mean this
        // assembly ships unverified.  Say so where a run that asked for
        // verification will see it: at debug level the one signal that the
        // check did not happen is indistinguishable from the check passing.
        REFOLD_LOG_WARN("assembly-verify",
                        "assembly NOT verified: the final source could not be "
                        "preprocessed for checking, so --verify-output has "
                        "nothing to compare and the result stands unchecked");
      else if (attempt > 0)
        REFOLD_LOG_INFO("assembly-verify",
                        "verified after {0} narrowing step(s)", attempt);
      return out;
    }

    // Name the smallest region owning each diverging region and rule out
    // preserving it.  An owner already ruled out means expanding it was not
    // enough, so widen to the region enclosing it.
    llvm::SmallVector<uint64_t, 8> owners;
    for (const std::pair<std::size_t, std::size_t> &range :
         verdict.divergentRanges) {
      std::optional<uint64_t> candidate =
          engine.FindSmallestOwnerForEditedToken(range.first);
      while (candidate && ownersMustExpand.count(*candidate))
        candidate = engine.FindEnclosingOwner(*candidate);
      if (candidate && !llvm::is_contained(owners, *candidate))
        owners.push_back(*candidate);
    }
    const std::optional<uint64_t> owner =
        owners.empty() ? std::nullopt : std::optional<uint64_t>(owners.front());

    // `fatal`: report and fail.  A theorem that mis-states which tokens it
    // realizes is a defect, and repairing it silently costs completeness in a
    // way nothing observes -- the output stays correct, so the broken theorem
    // survives.  `repair` opts into the conservative repair instead.
    if (verifyMode != OutputVerificationMode::Repair) {
      std::string owned = "<unattributed>";
      if (owner)
        owned = engine.DescribeOwner(*owner);
      return createStringError(
          std::make_error_code(std::errc::illegal_byte_sequence),
          "refolded source does not replay the edited preprocessed stream: "
          "%s; smallest region owning the divergence: %s. "
          "Re-run with --verify-output=repair to expand that region "
          "instead of failing",
          verdict.reason.c_str(), owned.c_str());
    }

    if (!owners.empty()) {
      // Same progress rule as the terminal ladder above: only a round that
      // gives up a region not already given up can change the next assembly.
      bool expandedNewOwner = false;
      for (uint64_t candidate : owners) {
        if (!ownersMustExpand.insert(candidate).second)
          continue;
        expandedNewOwner = true;
        REFOLD_LOG_INFO("assembly-verify", "  expanding {0}",
                        engine.DescribeOwner(candidate));
      }
      if (expandedNewOwner) {
        REFOLD_LOG_INFO("assembly-verify",
                        "unsound assembly: {0} diverging region(s), expanded "
                        "{1} owner(s) and retrying: {2}",
                        verdict.divergentRanges.size(), owners.size(),
                        verdict.reason);
        ++narrowingAttempts;
        continue;
      }
    }

    // Nothing narrower is left to give up: the divergence could not be
    // attributed, every enclosing owner is already expanded, or the ladder is
    // spent.  Take the carrier that reproduces the edited stream by
    // construction.
    REFOLD_LOG_WARN("assembly-verify",
                    "unsound assembly at edited token {0} could not be "
                    "narrowed further; expanding the translation unit: {1}",
                    static_cast<uint64_t>(verdict.mismatchTokenIndex),
                    verdict.reason);
    engine.terminalSink_.RequestTerminalFallback(
        withDivergingEditedToken(
            RefoldOwnerStateProof::SuffixStabilityTerminalFailureForComponent(
                OwnerStateComponent::LineNumber),
            verdict.mismatchTokenIndex),
        "assembly-verify",
        "assembled source does not replay the edited preprocessed stream");
    // The request recorded just above is part of this census, which is why the
    // carrier is taken after it rather than before.
    return takeTerminalCarrier(
        engine.expansionFallbackPlanner_->ResolvePostStructuralFallback(),
        engine.terminalSink_.Requests());
  }
}

void RefoldEngine::ProbeAlignmentResolution() {
  // Deliberately not routed through `Refold()`.  That entry point clears the
  // caller's materialized-mapping sidecar, and those mappings belong to the
  // attempt that will actually be emitted, not to a probe that emits nothing.
  stopAfterAlignmentResolution_ = true;
  terminalSink_.Reset();
  resetRefoldAttemptStats(lastStats_, model_);
  TheoremAudit().Reset();
  (void)RunRefoldPass();
}

std::string RefoldEngine::Refold() {
  if (materializedEditMappings_)
    materializedEditMappings_->clear();
  finalLineControlPruneCandidates_.clear();
  finalLineControlSourceMappings_.clear();

  // The engine first attempts structural refolding, then resolves the
  // post-structural fallback choice between proved intermediate expansion and
  // the explicit raw-B terminal carrier when proof discharge requests fallback.
  terminalSink_.Reset();
  resetRefoldAttemptStats(lastStats_, model_);
  TheoremAudit().Reset();

  std::string out = RunRefoldPass();

  // Once the structural pass finishes, any surviving theorem-audit violation
  // must be converted into the one explicit terminal fallback rather than
  // merely being reported.
  TheoremAudit().EnforceTheoremAuditInvariants();

  if (terminalSink_.HasRequest()) {
    out = expansionFallbackPlanner_->ResolvePostStructuralFallback();
    finalLineControlPruneCandidates_.clear();
    finalLineControlSourceMappings_.clear();
  }

  // Producer line-control and builtin-observer facts are not lowered into a
  // passive final-stream model here; generation sites have already attached
  // the compact obligation/removal proof records consumed by the pruner below.

  AuditFinalLineControlRemovalProofPopulation(
      TheoremAudit(), finalLineControlPruneCandidates_,
      "final-line-control-prune-candidates");

  FinalLineControlPruneResult finalLinePrune =
      PruneFinalLineControlDirectives(out, finalLineControlPruneCandidates_,
                                      finalLineControlValidationCallback_);
  AuditFinalLineControlAuthorityContract(
      TheoremAudit(), finalLinePrune.authority, "final-line-control-prune");

  if (finalLinePrune.changed) {
    auto mapPointAfterDeletion = [](uint64_t point, uint64_t begin,
                                    uint64_t end) -> uint64_t {
      const uint64_t size = end - begin;
      if (point <= begin)
        return point;
      if (point <= end)
        return begin;
      return point - size;
    };

    for (const FinalLineControlRemovedRange &removedRange :
         finalLinePrune.removedRanges) {
      if (materializedEditMappings_) {
        for (MaterializedEditMapping &mapping : *materializedEditMappings_) {
          mapping.refoldedSourceBegin = mapPointAfterDeletion(
              mapping.refoldedSourceBegin, removedRange.finalBegin,
              removedRange.finalEnd);
          mapping.refoldedSourceEnd = mapPointAfterDeletion(
              mapping.refoldedSourceEnd, removedRange.finalBegin,
              removedRange.finalEnd);
        }
      }

      AdjustFinalLineControlSourceMappingsAfterDeletion(
          finalLineControlSourceMappings_, removedRange.finalBegin,
          removedRange.finalEnd);
    }
  }

  out = finalLinePrune.output;

  // The assembly check in RunRefoldPass() ran against the text as assembled,
  // and the prune above has just deleted directives from it.  Whatever it
  // removed was therefore never checked: re-verify the text that will actually
  // be written, so a prune that changes how the source preprocesses cannot ship
  // behind a verdict that was reached before it ran.
  if (finalLinePrune.changed && finalAssemblyVerifier_) {
    const FinalAssemblyVerdict prunedVerdict =
        finalAssemblyVerifier_->Verify(out);
    if (!prunedVerdict.verified && !prunedVerdict.inconclusive) {
      REFOLD_LOG_WARN("assembly-verify",
                      "final line-control prune broke the assembly: {0}; "
                      "taking the terminal carrier instead",
                      prunedVerdict.reason);
      terminalSink_.RequestTerminalFallback(
          withDivergingEditedToken(
              RefoldOwnerStateProof::
                  SuffixStabilityTerminalFailureForComponent(
                      OwnerStateComponent::LineNumber),
              prunedVerdict.mismatchTokenIndex),
          "assembly-verify",
          "pruned assembly does not replay the edited preprocessed stream");
      out = expansionFallbackPlanner_->ResolvePostStructuralFallback();
      finalLineControlPruneCandidates_.clear();
      finalLineControlSourceMappings_.clear();
      if (materializedEditMappings_)
        materializedEditMappings_->clear();
    }
  }

  // Closing proof for newline-drift repairs, on the accepted assembly only.
  // A repair that could not be injected into its own replacement was deferred
  // to the next safe beginning-of-line, and no site along that path establishes
  // that the flush point precedes the observers the drift displaced.
  if (!AuditPreservedLineObserversInFinalOutput(out)) {
    // Record the request before falling back, exactly as the prune check above
    // does. The edited preprocessed stream is not an answer this engine may
    // return on its own: without a request the driver sees a successful
    // assembly, the closing verifier passes it because the stream replays
    // itself by construction, and a result that dropped every comment and
    // directive ships with a zero exit. Naming the failure is what routes it to
    // the one place allowed to decide there is no admissible refold.
    REFOLD_LOG_WARN("assembly-verify",
                    "preserved line observers were displaced in the final "
                    "output; requesting the terminal fallback rather than "
                    "returning the edited preprocessed stream");
    terminalSink_.RequestTerminalFallback(
        RefoldOwnerStateProof::SuffixStabilityTerminalFailureForComponent(
            OwnerStateComponent::LineNumber),
        "assembly-verify",
        "preserved line observers do not hold in the final output");
    out = expansionFallbackPlanner_->ResolvePostStructuralFallback();
    finalLineControlPruneCandidates_.clear();
    finalLineControlSourceMappings_.clear();
    if (materializedEditMappings_)
      materializedEditMappings_->clear();
  }

  emitRefoldAttemptStatsSummary(lastStats_, terminalSink_.HasRequest());
  emitTheoremAuditSummary(lastTheoremAudit_);
  return out;
}


bool RefoldEngine::ValidateTokenCount() {
  // Make sure that when we re-lex the A-stream tokens that it matches the token
  // count as listed in the refold map JSON file.
  if (static_cast<size_t>(model_.GetTokensCountA()) != aToks_.size()) {
    // A-side token-count disagreement means the refold map and the supplied
    // `--pp` replay file do not describe the same token stream.  One known
    // source of this shape is a producer that prints sideband pragma directive
    // text in the `.i` file while recording only the tokens produced after the
    // pragma has affected PP state.  There is no deterministic structural proof
    // we can discharge from that inconsistent A surface, so do not abort the
    // process.  Escape to the explicit terminal carrier (`--pp-mod`) instead.
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::ProducerFactsAvailable,
            TerminalFallbackFailureReason::MissingProducerFacts,
            TerminalFallbackFailureContext::ForStateComponent(
                "AStreamTokenCount")),
        "tok",
        llvm::formatv(
            "A-stream token count mismatch: model reported {0} tokens, but "
            "lexed sequence (aToks) has {1} tokens",
            model_.GetTokensCountA(), aToks_.size())
            .str());
    return false;
  }
  return true;
}

std::unique_ptr<llvm::MemoryBuffer>
RefoldEngine::LoadTUSource(StringRef tuPath) {
  if (tuSourceLoadError_)
    REFOLD_LOG_FATAL("src/load", "{0}", *tuSourceLoadError_);

  // Reuse the exact byte snapshot indexed during service-graph construction.
  // Re-reading the file here would permit a filesystem race in which
  // PlanTUByteSpan() proves ranges against one version of the TU while final
  // edit assembly applies them to another.  A private MemoryBuffer copy keeps
  // the existing caller lifetime contract while preserving byte identity with
  // the preprocessing-structure census.
  return MemoryBuffer::getMemBufferCopy(tuSourceBytes_, tuPath);
}

std::vector<diffutils::Hunk>
RefoldEngine::PlanTokenDiff(StringRef tuPath) {
  // PlanTokenDiff is the only phase boundary allowed to expose structural
  // hunks.  Re-entry would permit an insertion ledger or owner classifier to
  // retain indices into a stale normalization, so reject it as an internal
  // pipeline violation rather than trying to rebuild partially consumed state.
  if (structuralHunkPlanningPhase_ !=
      StructuralHunkPlanningPhase::NotStarted) {
    REFOLD_LOG_FATAL(
        "plan/order",
        "token-diff planning entered from invalid structural phase {0}",
        static_cast<unsigned>(structuralHunkPlanningPhase_));
  }

  // 1) Build the initial A/B token diff and refresh the per-run diff caches.
  //
  // RefoldTokenDiffPlanner owns lexeme mapping, LCS provenance construction,
  // normalized token-hunk derivation, A/B token map population, and raw
  // byte-hunk cache construction.  Owner-aware tiling starts from this
  // deterministic token-diff plan below.
  assert(tokenDiffPlanner_ && "token diff planner service not initialized");
  RefoldTokenDiffPlanner::TokenDiffPlan diffPlan =
      tokenDiffPlanner_->Plan(alignmentCertificationMemo_);

  // Retain the core theorem's forced map.  A forced anchor is an edge every
  // optimal path takes, so it is exactly what tells a hunk frontier this run
  // *chose* from one no realignment can move.  The demand check below reads it
  // rather than re-deriving anything.
  alignmentForcedMap_ = diffPlan.alignment.forcedMap;
  certificationWindowARanges_.clear();
  for (const diffutils::LcsCertificationWindow &window :
       diffPlan.alignment.certificationWindows) {
    if (window.IsCertified())
      certificationWindowARanges_.emplace_back(window.aBegin, window.aEnd);
  }

  // Production non-forced anchors must resolve to one durable semantic
  // witness retained by this engine run. Isolated candidate simulations carry
  // a temporary theorem-bearing override and are validated by the outer
  // resolver instead.
  if (!alignmentSelectionOverride_) {
    std::set<uint64_t> semanticWitnessIds;
    for (const AlignmentSemanticResolutionWitness &witness :
         alignmentSemanticResolutionWitnesses_) {
      if (witness.witnessId == 0 ||
          !semanticWitnessIds.insert(witness.witnessId).second) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "alignment semantic witness ledger has an invalid duplicate id");
      }
      if (!witness.completeEnumeration || witness.enumeratedMapCount == 0 ||
          witness.acceptedMapCount == 0 ||
          witness.acceptedMapCount + witness.rejectedMapCount !=
              witness.enumeratedMapCount ||
          witness.equivalenceKey.empty() ||
          witness.representativeMap.size() != abTokMapA2B_.size()) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "alignment semantic witness {0} is incomplete or malformed",
            witness.witnessId);
      }

      std::set<std::pair<uint64_t, uint64_t>> anchorEvidence;
      for (const AlignmentSemanticAnchorEvidence &evidence :
           witness.anchorEvidence) {
        if (evidence.aToken >= witness.representativeMap.size() ||
            witness.representativeMap[evidence.aToken] !=
                static_cast<int64_t>(evidence.bToken) ||
            !anchorEvidence.insert({evidence.aToken, evidence.bToken}).second) {
          REFOLD_LOG_FATAL(
              "lcs/semantic-resolver",
              "alignment semantic witness {0} has malformed anchor evidence",
              witness.witnessId);
        }
      }
      for (size_t aToken = 0; aToken < witness.representativeMap.size();
           ++aToken) {
        const int64_t bToken = witness.representativeMap[aToken];
        if (bToken < 0)
          continue;
        if (aToken < abTokAnchorProofs_.size()) {
          const diffutils::LcsAnchorProof &proof = abTokAnchorProofs_[aToken];
          if (proof.kind ==
              diffutils::LcsAnchorProofKind::CoreOptimalPathForced)
            continue;
          // Resolution is per certification window, so every witness records
          // the same complete-stream map but owes evidence only for the
          // anchors it proved. An anchor authorized by a different witness is
          // that witness's obligation and is checked against it below. With a
          // single witness this skips nothing: every non-forced anchor then
          // names that one witness.
          if (proof.kind == diffutils::LcsAnchorProofKind::
                                EquivalentNormalizedHunkAndOwner &&
              proof.semanticWitnessId != witness.witnessId)
            continue;
        }
        if (!anchorEvidence.count(
                {aToken, static_cast<uint64_t>(bToken)})) {
          REFOLD_LOG_FATAL(
              "lcs/semantic-resolver",
              "alignment semantic witness {0} lacks evidence for A-token {1}",
              witness.witnessId, aToken);
        }
      }
    }
    for (size_t aToken = 0; aToken < abTokAnchorProofs_.size(); ++aToken) {
      const diffutils::LcsAnchorProof &proof = abTokAnchorProofs_[aToken];
      if (proof.kind !=
          diffutils::LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner)
        continue;
      if (!alignmentSemanticTheoremActive_) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "A-token anchor {0} has semantic authority while the theorem "
            "boundary is inactive",
            aToken);
      }
      if (!semanticWitnessIds.count(proof.semanticWitnessId)) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "A-token anchor {0} names missing semantic witness {1}", aToken,
            proof.semanticWitnessId);
      }
      const auto witnessIt = llvm::find_if(
          alignmentSemanticResolutionWitnesses_,
          [&](const AlignmentSemanticResolutionWitness &witness) {
            return witness.witnessId == proof.semanticWitnessId;
          });
      if (witnessIt == alignmentSemanticResolutionWitnesses_.end() ||
          aToken >= witnessIt->representativeMap.size() ||
          witnessIt->representativeMap[aToken] != abTokMapA2B_[aToken]) {
        REFOLD_LOG_FATAL(
            "lcs/semantic-resolver",
            "A-token anchor {0} disagrees with semantic witness {1}", aToken,
            proof.semanticWitnessId);
      }
    }
  }

  std::vector<diffutils::Hunk> hunks = std::move(diffPlan.hunks);
  RetractHunkEdgesOutOfPartiallyOwnedMacroExpansions(hunks);
  structuralHunkPlanningPhase_ =
      StructuralHunkPlanningPhase::InitialTokenDiffBuilt;

  REFOLD_LOG_DEBUG(
      "plan",
      "refold model loaded: tu={0} includes={1} macroInvocations={2} "
      "ppTokenMapEntries={3}",
      tuPath, model_.GetIncludes().size(), model_.GetMacroInvocations().size(),
      model_.GetTokmapByPP().size());

  // 2) Normalize the initial hunk sequence before any owner-sensitive state is
  // built.  The generalized tiler may lower one structural deletion to several
  // ordinary token hunks, but downstream services continue to consume only the
  // normalized vector and its durable segment bindings.
  assert(mixedOwnerTilingPlanner_ &&
         "structural tiling planner service not initialized");
  RefoldMixedOwnerTilingPlanner::MixedOwnerTilingPlan structuralTilingPlan =
      mixedOwnerTilingPlanner_->Plan(std::move(hunks));
  hunks = std::move(structuralTilingPlan.hunks);

  // The planner writes the shared token-hunk cache and durable ledgers as part
  // of the same normalization transaction.  Validate that transaction before
  // insertion provenance records hunk indices; after that point a mismatch
  // could redirect claims or owner dispatch to the wrong segment.
  std::string structuralLedgerFailure;
  const bool structuralLedgersAgree =
      structuralTilingLedgersMatchNormalizedHunks(
          hunks, mixedOwnerTilingWitnesses_,
          mixedOwnerTilingSegmentBindings_, structuralLedgerFailure);
  if (abTokHunks_ != hunks ||
      structuralTilingPlan.mixedOwnerWitnessCount !=
          mixedOwnerTilingWitnesses_.size() ||
      structuralTilingPlan.segmentBindingCount !=
          mixedOwnerTilingSegmentBindings_.size() ||
      !structuralLedgersAgree) {
    REFOLD_LOG_FATAL(
        "plan/order",
        "structural tiling did not atomically publish its normalized hunks "
        "and ledgers: returnedHunks={0} cachedHunks={1} "
        "reportedWitnesses={2} durableWitnesses={3} "
        "reportedBindings={4} durableBindings={5} detail={6}",
        hunks.size(), abTokHunks_.size(),
        structuralTilingPlan.mixedOwnerWitnessCount,
        mixedOwnerTilingWitnesses_.size(),
        structuralTilingPlan.segmentBindingCount,
        mixedOwnerTilingSegmentBindings_.size(), structuralLedgerFailure);
  }
  structuralHunkPlanningPhase_ =
      StructuralHunkPlanningPhase::StructuralTilingComplete;

  if (inDebugMode()) {
    size_t insertOnlyHunks = 0;
    size_t deleteOnlyHunks = 0;
    size_t replaceHunks = 0;
    for (const auto &h : hunks) {
      if (h.isInsertOnly()) {
        ++insertOnlyHunks;
      } else if (h.bStart >= h.bEnd) {
        ++deleteOnlyHunks;
      } else {
        ++replaceHunks;
      }
    }
    debug("diff",
          "token diff normalized: hunks={0} replacements={1} insertions={2} "
          "deletions={3} structuralTilingWitnesses={4}",
          hunks.size(), replaceHunks, insertOnlyHunks, deleteOnlyHunks,
          mixedOwnerTilingWitnesses_.size());
  }

  // 3) Build provenance from the normalized sequence only.  In particular, a
  // structural deletion lowered to A=[a0,a1), B=[q,q) segments must not leave
  // insertion or claim indices referring to the original unsplit hunk.
  if (structuralHunkPlanningPhase_ !=
      StructuralHunkPlanningPhase::StructuralTilingComplete) {
    REFOLD_LOG_FATAL("plan/order",
                     "insertion provenance requested before structural "
                     "tiling completed");
  }
  BInsertionLedger().BuildProvenance(hunks);
  BInsertionLedger().PreclaimStandaloneInsertions(tuPath, hunks);
  structuralHunkPlanningPhase_ =
      StructuralHunkPlanningPhase::InsertionLedgerReady;
  return hunks;
}

void RefoldEngine::TraceStructuralHunkEnvelopes(
    ArrayRef<diffutils::Hunk> hunks) {
  // Validate B-token envelopes for every hunk. In trace mode, also show the
  // exact B-side fragment that later owner/macro/include proofs must explain.
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // Case A: The hunk is logically empty (e.g., a pure deletion)
    if (h.bStart >= h.bEnd) {
      continue;
    }

    // Case B: Hunk indices are out of bounds for the token-to-byte map
    if (h.bEnd >= bTokOff_.size()) {
      REFOLD_LOG_FATAL(
          "hunks", "#{0} {1:verbose} B=OUT-OF-BOUNDS: h.bEnd={2} map.size={3}",
          i, h, h.bEnd, bTokOff_.size());
      continue;
    }

    const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];
    const size_t b1 = bTokOff_[static_cast<size_t>(h.bEnd)];

    // Case C: The token-to-byte map contains sentinels (virtual/synthetic
    // tokens)
    if (b0 == StringRef::npos || b1 == StringRef::npos) {
      REFOLD_LOG_FATAL("hunks", "#{0} {1:verbose} B=SENTINEL: b0={2} b1={3}", i,
                       h, (b0 == StringRef::npos ? "npos" : "valid"),
                       (b1 == StringRef::npos ? "npos" : "valid"));
      continue;
    }

    // Case D: Byte offsets are inverted (corrupt map or out-of-order tokens)
    if (b1 < b0) {
      REFOLD_LOG_FATAL("hunks",
                       "#{0} {1:verbose} B=INVERTED-OFFSETS: b0={2} b1={3}", i,
                       h, b0, b1);
      continue;
    }

    // Final physical safety clamp (prevents crashes if map is stale relative to
    // source)
    const size_t lo = std::min(b0, bSource_.size());
    const size_t hi = std::min(b1, bSource_.size());

    if (inTraceMode()) {
      StringRef bfrag = bSource_.substr(lo, hi - lo);
      trace("diff/hunk",
            "hunk #{0}: aTokens=[{1},{2}) bTokens=[{3},{4}) bBytes=[{5},{6}) "
            "bText=\"{7}\"",
            i, h.aStart, h.aEnd, h.bStart, h.bEnd, lo, hi,
            stringutils::showWsWithClip(bfrag, 160));
    }
  }
}

bool RefoldEngine::StageSidebandEdits(
    StringRef tuPath, StringRef tuBytes,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  // Sideband pragmas are staged before ordinary structural dispatch so later
  // TU/include/macro realization can strip or avoid any separately-owned
  // sideband replay bytes from ordinary hunk replacements.
  return appendSidebandPragmaSourceEdits(
      sidebandPragmaEdits_, model_, tuPath, tuBytes, pathIdentity_, *textEditAssembler_,
      ProofLattice(), terminalSink_, structuralHunkDispatcher);
}

void RefoldEngine::RetractHunkEdgesOutOfPartiallyOwnedMacroExpansions(
    std::vector<diffutils::Hunk> &hunks) const {
  for (diffutils::Hunk &hunk : hunks) {
    if (hunk.aStart >= hunk.aEnd)
      continue;

    // Left edge: advance past the expansion it sits inside.  Each step gives
    // one leading token back to the untouched region on the left, so the token
    // must be identical on both sides for that region to reproduce it.
    while (hunk.aStart < hunk.aEnd && hunk.bStart < hunk.bEnd) {
      const RefoldModel::PPSpan *span = macroExpansionStraddledAtEdge(
          model_, hunk.aStart, hunk.aEnd, /*edgeIsLeft=*/true);
      if (!span)
        break;
      if (!aAndBTokensAreIdentical(aToks_, bToks_, hunk.aStart, hunk.bStart))
        break;
      ++hunk.aStart;
      ++hunk.bStart;
    }

    // Right edge: retreat before the expansion, giving trailing tokens back to
    // the untouched region on the right under the same identity requirement.
    while (hunk.aStart < hunk.aEnd && hunk.bStart < hunk.bEnd) {
      const RefoldModel::PPSpan *span = macroExpansionStraddledAtEdge(
          model_, hunk.aEnd, hunk.aStart, /*edgeIsLeft=*/false);
      if (!span)
        break;
      if (!aAndBTokensAreIdentical(aToks_, bToks_, hunk.aEnd - 1,
                                   hunk.bEnd - 1)) {
        break;
      }
      --hunk.aEnd;
      --hunk.bEnd;
    }
  }
}

bool RefoldEngine::DispatchStructuralHunks(
    StringRef tuPath, StringRef tuBytes, ArrayRef<diffutils::Hunk> hunks,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  // Owner classification, direct TU planning, and macro/include selection must
  // all observe exactly the hunk sequence used to build insertion provenance.
  // This guard prevents a future orchestration change from dispatching the
  // initial pre-tiling diff or from bypassing insertion-ledger construction.
  const bool dispatchMatchesNormalizedCache =
      hunks.size() == abTokHunks_.size() &&
      std::equal(hunks.begin(), hunks.end(), abTokHunks_.begin());
  if (structuralHunkPlanningPhase_ !=
          StructuralHunkPlanningPhase::InsertionLedgerReady ||
      !dispatchMatchesNormalizedCache) {
    REFOLD_LOG_FATAL(
        "plan/order",
        "structural owner dispatch observed an uncommitted hunk plan: "
        "phase={0} dispatchedHunks={1} cachedHunks={2}",
        static_cast<unsigned>(structuralHunkPlanningPhase_), hunks.size(),
        abTokHunks_.size());
  }

  // Local lexical predicate used by the theorem-lattice tie-breaker below.
  // A top-level comma in replacement text would split the original invocation
  // argument list, so the otherwise-equivalent TU edit must stay behind the
  // macro reconstruction proof path.
  auto hasTopLevelCommaInReplacement = [&](StringRef text) -> bool {
    return refoldMacroActualHasTopLevelComma(text, lexLang_);
  };

  auto theoremLatticePrefersExactTUArgEditOverMacroArgsOnly =
      [&](const RefoldModel::MacroInvocation &macro,
          const diffutils::Hunk &hunk,
          const MacroPatch &macroCandidate) -> bool {
    // Cross-domain theorem-lattice tie-breaker:
    //
    // The same PP hunk can sometimes be represented in two valid ways:
    //
    //   1. a macro args-only rewrite, e.g. ID(/*keep*/ 1) -> ID(2)
    //   2. an exact TU byte edit inside the original argument spelling,
    //      e.g. ID(/*keep*/ 1) -> ID(/*keep*/ 2)
    //
    // The named tie-breaker may prefer the TU edit when it is provably
    // equivalent, because it preserves callsite trivia that macro argument
    // reconstruction does not retain. This is intentionally not a general "TU
    // beats macro" rule; it applies only to a single direct argument occurrence
    // whose source spelling can be edited without changing macro arity or
    // expansion multiplicity.
    //
    // Only compete against an already-proven, structure-preserving args-only
    // macro candidate. Other macro proofs have their own stronger invariants
    // and should not be displaced by this narrow preference.
    if (macroCandidate.proof.kind != MacroPatchProofKind::ArgsOnlyStandard ||
        !macroCandidate.proof.preservesInvocationStructure)
      return false;

    const AcceptedResultCandidate macroAccepted =
        ProofLattice().AcceptedCandidateBuilder().BuildAcceptedMacroCandidate(
            macroCandidate);
    if (!ProofLattice()
             .AcceptedResultRanker()
             .IsSelectableAcceptedResultCandidate(macroAccepted))
      return false;

    // This preference rewrites existing source bytes inside an argument. Pure
    // insertions/deletions and empty B ranges need different anchoring rules.
    if (!hunk.isReplace() || hunk.aStart >= hunk.aEnd ||
        hunk.bStart >= hunk.bEnd)
      return false;

    // The invocation itself must be spelled in the main TU. If the invocation
    // is include-owned or lacks callsite provenance, rewriting its argument
    // bytes here would cross source ownership boundaries.
    if (!macro.invB || !macro.invE || !macro.invFile ||
        !pathIdentity_.PathsEqual(*macro.invFile, tuPath))
      return false;

    // The PP hunk must map back to a concrete TU token range. This prevents
    // choosing a TU-byte edit for tokens that only exist through macro body
    // spelling, include materialization, or another non-TU source.
    if (!OwnerClassifier().HunkMapsToTU(hunk.aStart, hunk.aEnd, tuPath))
      return false;

    auto spanPlan =
        TUEditPlanner().PlanTUByteSpan(hunk.aStart, hunk.aEnd, tuPath);
    if (!spanPlan || spanPlan->tuByteBegin >= spanPlan->tuByteEnd)
      return false;
    auto span = spanPlan->byteRange();

    // The concrete source bytes for the hunk must lie inside the invocation
    // callsite. Otherwise the edit is not an argument-local alternative to the
    // macro candidate.
    if (span.first < *macro.invB || span.second > *macro.invE)
      return false;

    std::optional<uint32_t> touchedArgIdx;
    for (const auto &occ : macro.argSpans) {
      if (occ.kind != PPArgSpanKind::Standard)
        continue;

      // Find the single STANDARD expansion occurrence that covers the PP hunk.
      // If more than one argument occurrence covers it, the hunk is ambiguous
      // and must stay on the existing macro proof path.
      if (occ.begin <= hunk.aStart && hunk.aEnd <= occ.end) {
        if (touchedArgIdx)
          return false;
        touchedArgIdx = occ.argIdx;
      }
    }
    if (!touchedArgIdx)
      return false;

    unsigned directStandardOccurrences = 0;
    for (const auto &occ : macro.argSpans) {
      if (occ.argIdx != *touchedArgIdx)
        continue;

      // Editing the argument source changes every expansion of that formal in
      // this invocation. Therefore the preference is sound only when the formal
      // appears exactly once, and only as a direct STANDARD substitution.
      if (occ.kind != PPArgSpanKind::Standard)
        return false;
      ++directStandardOccurrences;
    }

    // Stringify and paste uses transform the argument spelling before it
    // reaches the PP output. A direct TU argument edit is not equivalent to an
    // args-only reconstruction for those cases.
    for (const auto &occ : macro.stringifySpans) {
      if (occ.argIdx == *touchedArgIdx)
        return false;
    }
    for (const auto &occ : macro.pasteSpans) {
      if (occ.argIdx == *touchedArgIdx)
        return false;
    }

    if (directStandardOccurrences != 1)
      return false;

    if (*touchedArgIdx >= macro.invArgRanges.size())
      return false;
    const auto &argRange = macro.invArgRanges[*touchedArgIdx];
    if (!argRange.first || !argRange.second ||
        *argRange.second < *argRange.first) {
      return false;
    }

    // The source byte span must be contained in the original argument spelling,
    // not merely somewhere inside the invocation parentheses.
    if (span.first < *argRange.first || span.second > *argRange.second)
      return false;

    StringRef replacement = refoldSliceExactTokenCoverage(
        bTokOff_, bToks_, bSource_, hunk.bStart, hunk.bEnd);

    // Replacing an argument subrange with a top-level comma would split the
    // original invocation argument list. That is a semantic arity change, so it
    // must remain on the macro reconstruction/fallback path.
    if (hasTopLevelCommaInReplacement(replacement))
      return false;

    // This preference is only a tie-breaker for the case where the macro patch
    // and the direct source edit spell the same invocation.  Empty-slot and
    // __VA_OPT__ repairs can move tokens between formals while still preserving
    // the macro call; deferring those patches to a literal TU argument edit
    // produces a validating but less structural result such as
    // `MAYBE_PLUS(3 + 4)` instead of `MAYBE_PLUS(3, 4)`.  Project the TU hunk
    // back into the original invocation spelling and require byte-equivalence
    // with the accepted macro patch before letting the TU edit compete.  If the
    // two spellings differ, the macro proof is carrying structure that the TU
    // byte edit would erase, so the TU edit is not a harmless substitute.
    if (!macro.invText || !macro.invB || span.second < span.first ||
        span.first < *macro.invB) {
      return false;
    }
    const uint64_t relBegin64 = span.first - *macro.invB;
    const uint64_t relEnd64 = span.second - *macro.invB;
    if (relEnd64 < relBegin64 || relEnd64 > macro.invText->size())
      return false;
    std::string directEditInvocation = stringutils::replaceRange(
        macro.invText->str(), static_cast<size_t>(relBegin64),
        static_cast<size_t>(relEnd64), replacement.str());
    if (StringRef(directEditInvocation).trim() !=
        StringRef(macroCandidate.replacement).trim()) {
      return false;
    }

    AcceptedResultCandidate tuCandidate =
        ProofLattice()
            .AcceptedCandidateBuilder()
            .BuildAcceptedTUTextEditCandidate(
                AcceptedPathKind::TUByteSpanMappedEdit, hunk, *spanPlan,
                /*structuralBinding=*/nullptr, replacement);
    tuCandidate.proofSummary.selectionTieBreaker =
        TheoremSelectionTieBreakerKind::
            ExactTUArgumentEditOverEquivalentMacroArgsOnly;

    const bool latticePrefersTU =
        ProofLattice()
            .AcceptedResultRanker()
            .IsSelectableAcceptedResultCandidate(tuCandidate) &&
        ProofLattice().AcceptedResultRanker().LatticePrefers(
            tuCandidate.proofSummary, macroAccepted.proofSummary) &&
        !ProofLattice().AcceptedResultRanker().LatticePrefers(
            macroAccepted.proofSummary, tuCandidate.proofSummary);
    if (!latticePrefersTU)
      return false;

    return true;
  };

  // Iterate over all hunks:
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // These shape predicates are semantic control-flow inputs, not trace
    // payload. Keep them close to the dispatch decision that consumes them.
    const bool isIns = h.isInsertOnly();
    const bool isDel = h.isDeleteOnly();

    // a) Segment-aware owner classification: this decides TU vs include vs “no
    // segment”.
    Owner owner = OwnerClassifier().ClassifyOwnerWithSegments(tuPath, h);

    // b) Macro call-site still has priority over TU/include.  Half-open
    // boundary insertions are not macro-owned by default; macro-domain boundary
    // selectors expose only the narrow insertion-at-cover-edge candidates that
    // a later whole-cover/definition-tape proof must still validate before the
    // hunk is absorbed into a callsite rewrite.
    const RefoldModel::MacroInvocation *macroTarget =
        macroTopology_.SmallestCoveringPatchableMacro(h.aStart, h.aEnd,
                                                      owner.includeId);
    if (!macroTarget && isIns)
      macroTarget = macroBoundarySelector_.RightBoundaryVaOptActivationMacro(
          h.aStart, owner.includeId);
    if (!macroTarget && isIns)
      macroTarget = macroBoundarySelector_.BoundaryGeneratedSelectorMacro(
          h.aStart, owner.includeId);
    if (!macroTarget && isIns)
      macroTarget = macroBoundarySelector_.BoundaryDefinitionTapeReplayMacro(
          h, owner.includeId);
    if (auto *m = macroTarget) {
      if (m->invB && m->invE) {
        bool appliedMacroPatch = false;
        for (const RefoldModel::MacroInvocation *target = m;
             target != nullptr;) {
          RefoldStructuralHunkDispatcher::MacroPatchStagingSlot stagingSlot =
              structuralHunkDispatcher.PrepareMacroPatchStagingSlot(*target);
          RefoldMacroPatchPlanner::ExistingMacroPatchContext
              existingPatchContext;
          existingPatchContext.patch = stagingSlot.existingPatch;
          existingPatchContext.isCallsite = stagingSlot.existingIsCallsite;

          // Try to build a whole-cover replacement at the current target
          // invocation. If successful, install/replace the callsite patch for
          // this macro id and stop climbing the caller chain.
          auto updated =
              MacroPatchPlanner().BuildMacroInvocationPatchWholeCover(
                  *target, h, stagingSlot.currentInvocationText,
                  structuralHunkDispatcher.MacroPatchMergeBucketsForPlanner(),
                  existingPatchContext);
          if (updated) {
            if (!stagingSlot.existingPatch &&
                theoremLatticePrefersExactTUArgEditOverMacroArgsOnly(
                    *target, h, *updated)) {
              break;
            }

            const Owner currentPatchOwner =
                MacroPatchPlanner().NormalizeHunkOwnerForPatch(tuPath, h);
            if (stagingSlot.existingPatch)
              MacroPatchPlanner().CarryMacroPatchOwnerCertificate(
                  *updated, *stagingSlot.existingPatch);
            structuralHunkDispatcher.MergeMaterializedBTokenRangeFromSlot(
                *updated, stagingSlot, h);
            MacroPatchPlanner().CertifyMacroPatchOwnerWitness(
                *updated, currentPatchOwner);
            updated->macroId = stagingSlot.patchKey;
            structuralHunkDispatcher.StageMacroPatch(stagingSlot,
                                                     std::move(*updated));
            appliedMacroPatch = true;
            break;
          }

          // No deterministic whole-cover patch at this child invocation.
          // Climb to the immediate caller macro and retry at that enclosing
          // callsite, so the edit can be represented at a higher macro layer
          // if needed.
          if (!target->callerMacroId)
            break;
          const RefoldModel::MacroInvocation *parent =
              macroTopology_.FindMacroInvocationById(*target->callerMacroId);
          if (!parent)
            break;
          target = parent;
        }

        if (appliedMacroPatch)
          continue;
      }
    }

    // c) Special case: pure insertions exactly at the boundary between sibling
    // includes that share a common parent. In this case, per policy, the
    // insertion should be attached to the *parent* include so that the
    // refolded C places it between `#include` lines, not inside any child.
    if (isIns) {
      const RefoldModel::IncludeItem *parentBoundaryInc = nullptr;
      if (auto boundaryPlan =
              TUEditPlanner().FindBoundaryParentIncludeForPureInsertion(h))
        parentBoundaryInc = model_.GetIncludeById(boundaryPlan->includeId);
      if (parentBoundaryInc) {
        IncludePatch patch =
            IncludeInsertionPlanner().BuildIncludeInsertionPatch(
                *parentBoundaryInc, h);
        patch.condArm.present = owner.condArmId.has_value();
        patch.condArm.armId = owner.condArmId.value_or(0);
        structuralHunkDispatcher.AddIncludePatch(parentBoundaryInc,
                                                 std::move(patch));
        continue;
      }

      if (owner.kind == OwnerKind::Include && owner.includeId) {
        std::optional<uint64_t> firstCond =
            model_.FirstConditionalArmStartA(*owner.includeId);
        if (firstCond && h.aStart <= *firstCond) {
          const RefoldModel::IncludeItem *inc =
              model_.GetIncludeById(*owner.includeId);
          // NOTE: `inc` cannot be null if owner has an `includeId`
          IncludePatch patch =
              IncludeInsertionPlanner().BuildIncludeInsertionPatch(*inc, h);
          patch.condArm.present = owner.condArmId.has_value();
          patch.condArm.armId = owner.condArmId.value_or(0);
          structuralHunkDispatcher.AddIncludePatch(inc, std::move(patch));
          continue;
        }
      }
    }

    // d) Include-owned edit (segment policy already enforced in
    // RefoldOwnerClassifier).
    if (owner.kind == OwnerKind::Include && owner.includeId) {
      const RefoldModel::IncludeItem *inc =
          model_.GetIncludeById(*owner.includeId);
      // NOTE: `inc` cannot be null if owner has an `includeId`
      IncludePatch patch =
          IncludeInsertionPlanner().BuildIncludeInsertionPatch(*inc, h);
      patch.condArm.present = owner.condArmId.has_value();
      patch.condArm.armId = owner.condArmId.value_or(0);
      structuralHunkDispatcher.AddIncludePatch(inc, std::move(patch));
      continue;
    }

    // e) TU edit? (segments + existing TU mapping cooperate here).
    // We rely on the token→file map as the source of truth for TU ownership.
    // Segment classification is only used to detect include-owned edits; it
    // should not force a hunk into the TU if any mapped token belongs to a
    // header. So only treat it as TU when the hunk map says so.
    bool mapsToTU = OwnerClassifier().HunkMapsToTU(h.aStart, h.aEnd, tuPath);

    if (owner.kind == OwnerKind::TU && !mapsToTU) {
      // Deterministic rule: TU ownership must be supported by provenance. If
      // tokmap-based evidence does not indicate TU ownership, do not force TU
      // edits (even for insertions). Leave owner unresolved so strict mode can
      // surface the deficiency.
      owner = Owner::Unknown();
    }

    if (mapsToTU) {
      auto spanPlan =
          TUEditPlanner().PlanTUByteSpan(h.aStart, h.aEnd, tuPath); // [b,e)
      if (spanPlan) {
        auto span = spanPlan->byteRange();

        std::string repl;
        const uint64_t rawTUStart = span.first;
        const uint64_t rawTUEnd = span.second;
        std::optional<uint64_t> materializedBByteBegin;
        std::optional<uint64_t> materializedBByteEnd;
        if (auto bBytes =
                sourceMapper_.BTokenRangeToByteRange(h.bStart, h.bEnd)) {
          materializedBByteBegin = bBytes->first;
          materializedBByteEnd = bBytes->second;
        }

        if (h.bStart < h.bEnd) {
          StringRef bSlice =
              h.isInsertOnly()
                  ? refoldSliceTokenEnvelope(bTokOff_, bSource_, h.bStart,
                                             h.bEnd)
                  : refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                                  h.bStart, h.bEnd);
          repl.assign(bSlice.data(), bSlice.data() + bSlice.size());
          if (h.isInsertOnly()) {
            // Pure insertions use the token envelope, which may include
            // zero-normal-token sideband directive lines adjacent to the
            // ordinary inserted tokens.  If a sideband replacement/deletion is
            // also proved and materialized through its owning include/TU atom,
            // the ordinary insertion must not replay the same B bytes.
            std::optional<uint64_t> envelopeBegin;
            std::optional<uint64_t> envelopeEnd;
            if (!bSlice.empty()) {
              envelopeBegin =
                  static_cast<uint64_t>(bSlice.data() - bSource_.data());
              envelopeEnd =
                  *envelopeBegin + static_cast<uint64_t>(bSlice.size());
            }
            repl = textEditAssembler_->StripSeparatelyOwnedSidebandReplay(
                repl, envelopeBegin, envelopeEnd);
          }
          const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];

          // Token-envelope byte ranges begin at the first inserted token, so
          // they do not include any spaces or tabs that appear immediately
          // before that token in B on the same line. For a zero-width TU
          // insertion, preserve those preceding spaces or tabs when forming
          // the inserted text, unless equivalent spacing is already present
          // immediately to the left of the insertion point in the TU.
          if (span.first == span.second && h.bStart > 0) {
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
                  (span.first > 0 && (tuBytes[span.first - 1] == ' ' ||
                                      tuBytes[span.first - 1] == '\t'));
              if (!hasSpaceLeft) {
                repl.insert(0, std::string(bSource_.data() + p, b0 - p));
                materializedBByteBegin = static_cast<uint64_t>(p);
              }
            }
          }
        }

        bool consumedSeparatorGapForPunctuation = false;

        if (h.isInsertOnly()) {
          // B attaches the inserted content to its left neighbour iff there is
          // no whitespace before the first inserted B token.
          const size_t bStartIdx = static_cast<size_t>(h.bStart);
          const bool replayAttachesLeftInB =
              bStartIdx < bTokOff_.size() && bTokOff_[bStartIdx] > 0 &&
              !stringutils::isWs(bSource_[bTokOff_[bStartIdx] - 1]);
          consumedSeparatorGapForPunctuation =
              maybeConsumeLeftSourceGapWhenBAttaches(
                  model_, pathIdentity_, tuPath, tuBytes, span, StringRef(repl),
                  replayAttachesLeftInB, lexLang_);
        }

        TUEditPlanner().MaybeExtendTUSpanOverClosedTrailingCallSuffix(
            h, tuPath, tuBytes, repl, span);

        bool advancedOverSourceLineControlPrefix =
            maybeAdvanceTUInsertionPastSourceLineControlPrefix(
                TUEditPlanner(), lineControlProof_, h, tuPath, tuBytes, span);
        std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment;
        if (advancedOverSourceLineControlPrefix) {
          insertionAnchorAdjustment = TUInsertionAnchorAdjustment{
              TUInsertionAnchorAdjustmentKind::SourceLineControlPrefix,
              rawTUStart, span.first};
        }

        // Is this span replacing a TU "gap" (bytes that are all whitespace)?
        std::string original;
        if (span.second > span.first) {
          original.assign(tuBytes.data() + span.first,
                          tuBytes.data() + span.second);
        } else if (span.second < span.first) {
          REFOLD_LOG_FATAL("tu/span", "invalid TU byte span: [{0},{1})",
                           span.first, span.second);
        }

        bool replacingGap = !original.empty() && stringutils::isWs(original);

        // If we’re replacing a non-empty TU gap and the inserted text doesn’t
        // start with WS, prefix EXACTLY ONE space from the gap to preserve
        // “return injected” (no double spaces).
        if (replacingGap && !consumedSeparatorGapForPunctuation &&
            !repl.empty() && !stringutils::isWs(repl.front())) {
          repl.insert(repl.begin(), ' ');
        }

        // Final boundary spacing fixup:
        // - On the left, only let refoldPadAtBoundaries add a space if we did
        //   not already preserve whitespace from a replaced TU gap; otherwise
        //   we could duplicate spacing.
        // - On the right, always allow padding if the replacement would
        //   otherwise glue to the following TU text.
        // Keep a copy for logging; refoldPadAtBoundaries consumes via move.
        std::string rawRepl = repl;

        std::string padded = refoldPadAtBoundaries(
            tuBytes, static_cast<size_t>(span.first),
            static_cast<size_t>(span.second), std::move(repl),
            /*allowLeft*/ !replacingGap, /*allowRight*/ true, lexLang_);

        const bool skipLocalResync =
            tuInsertionBeforeMaterializedInclude(
                TUEditPlanner(), model_, sidebandPragmaEdits_, h, tuPath, span,
                /*requireVisibleReplayText=*/true) ||
            lineControlProof_.TUInsertionCanDeferResyncToConditionalJoin(
                advancedOverSourceLineControlPrefix, tuPath, span.second);

        ResyncOutcome ro =
            skipLocalResync
                ? ResyncOutcome(padded, std::nullopt)
                : textEditAssembler_->ApplyResyncOrPend(
                      tuBytes, span.first, span.second, padded, tuPath);
        if (std::optional<TextEdit> directEdit =
                textEditAssembler_->BuildDirectTUHunkTextEdit(
                    h, i, span, std::move(ro), StringRef(padded), rawTUStart,
                    rawTUEnd, materializedBByteBegin, materializedBByteEnd,
                    AcceptedPathKind::TUByteSpanMappedEdit,
                    std::move(insertionAnchorAdjustment))) {
          structuralHunkDispatcher.AddTUEdit(std::move(*directEdit));
          continue;
        }
      }
    }

    // f) Ownership resolution failed.
    //
    // Treat unresolved ownership as a theorem-boundary case rather than a hard
    // internal error: once macro ownership, include ownership, and truthful
    // TU ownership all fail, the engine may still salvage the edit via a
    // declared TU byte-span class. If that last deterministic TU realization
    // also fails, the code below requests the named terminal fallback
    // OwnerUnresolvedNoTUAnchor.
    //
    // Do not abort in strict mode here. Strict mode is enforced later by the
    // theorem audit and terminal-fallback machinery, not by crashing before
    // the explicit out-of-domain boundary is recorded.

    auto hunkConsumesNonTUMappedToken = [&]() -> bool {
      const auto &tokmapByPP = model_.GetTokmapByPP();
      for (uint64_t pp = h.aStart; pp < h.aEnd; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it == tokmapByPP.end())
          continue;
        if (!pathIdentity_.PathsEqual(it->second.file, tuPath))
          return true;
      }
      return false;
    };

    // A conservative TU byte-span edit may only be used when the whole consumed
    // hunk is TU-owned.  If the hunk also consumes tokens produced by a
    // top-level include, replacing only the TU subset would leave the original
    // include directive alive and replay stale tokens.  That hybrid case routes
    // through the declared TUIncludeClosureEdit proof; otherwise fail closed
    // instead of manufacturing an unsound partial TU edit.
    if (hunkConsumesNonTUMappedToken()) {
      llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8>
          stagedSourceIntervals =
              structuralHunkDispatcher.BuildTUClosureSourceIntervals();
      if (auto closureEdit =
              expansionFallbackPlanner_
                  ->BuildTUIncludeClosureEditForUnresolvedHunk(
                      h, tuPath, tuBytes, stagedSourceIntervals)) {
        structuralHunkDispatcher.AddTUEdit(std::move(*closureEdit));
        continue;
      }

      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::OwnerClosedCover,
              TerminalFallbackFailureReason::NoOwnerClosedCover,
              TerminalFallbackFailureContext::ForHunkTokenEnvelope(
                  i, h.aStart, h.aEnd, h.bStart, h.bEnd)),
          "classify",
          ProofLattice().BuildOwnerUnresolvedNoTUAnchorDetail(i, h, tuPath,
                                                              owner, mapsToTU));
      continue;
    }

    if (auto spanPlan = TUEditPlanner().PlanTUByteSpan(h.aStart, h.aEnd,
                                                       tuPath)) { // [b, e)
      if (std::optional<TextEdit> directEdit = BuildDirectTUByteSpanEditForHunk(
              h, i, isDel, tuPath, tuBytes, spanPlan->byteRange())) {
        structuralHunkDispatcher.AddTUEdit(std::move(*directEdit));
        continue;
      }
    }

    // Before we escalate to the explicit terminal out-of-domain carrier, try
    // the declared TUIncludeClosureEdit class.  This source-closure proof keeps
    // already-proved structural work alive by materializing a closed run of
    // top-level TU `#include` directives directly into TU source text when that
    // run either exactly covers the unresolved PP hunk or can be widened to the
    // full include cover without absorbing another token diff.
    llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8> stagedSourceIntervals =
        structuralHunkDispatcher.BuildTUClosureSourceIntervals();
    if (auto closureEdit = expansionFallbackPlanner_
                               ->BuildTUIncludeClosureEditForUnresolvedHunk(
                                   h, tuPath, tuBytes, stagedSourceIntervals)) {
      structuralHunkDispatcher.AddTUEdit(std::move(*closureEdit));
      continue;
    }

    // The hunk has no realization of its own.  If its owner was already ruled
    // out from keeping its callsite, emit that owner's expansion here rather
    // than giving up the translation unit.
    //
    // Marking an owner must-expand tells the macro selector not to preserve the
    // callsite, but nothing else then emits the invocation's expansion at that
    // site: for an include, materialization supplies the body, while a
    // TU-level invocation has no such step.  The hunk therefore fails again on
    // every retry, and widening dead-ends because a TU-level invocation has
    // neither a caller nor an owning include -- so the ladder reaches the
    // whole-file carrier with the region it named still unrealized.  Staging
    // the invocation's proved whole-cover replacement closes that gap: the
    // region is expanded exactly where it failed and every other hunk keeps
    // its refolded realization.
    if (std::optional<uint64_t> failingOwner =
            FindSmallestOwnerForAToken(h.aStart)) {
      if (ownersMustExpand_.count(*failingOwner)) {
        if (const RefoldModel::MacroInvocation *invocation =
                macroTopology_.FindMacroInvocationById(*failingOwner)) {
          if (invocation->invB && invocation->invE) {
            if (std::optional<WholeCoverPlan> wholeCoverPlan =
                    MacroPatchPlanner().ComputeWholeCoverPlan(*invocation)) {
              MacroPatch wholeCoverPatch{*invocation->invB, *invocation->invE,
                                         wholeCoverPlan->clippedText,
                                         invocation->id};
              ProofLattice().CertifyMacroWholeCoverRealizationPatch(
                  wholeCoverPatch, *wholeCoverPlan, *invocation);
              MacroPatchPlanner().CertifyMacroPatchOwnerWitness(
                  wholeCoverPatch,
                  invocation->ownerIncludeId
                      ? Owner::Include(*invocation->ownerIncludeId)
                      : Owner::TU());
              RefoldStructuralHunkDispatcher::MacroPatchStagingSlot slot =
                  structuralHunkDispatcher.PrepareMacroPatchStagingSlot(
                      *invocation);
              structuralHunkDispatcher.StageMacroPatch(
                  slot, std::move(wholeCoverPatch));
              REFOLD_LOG_INFO(
                  "classify",
                  "hunk A=[{0},{1}) has no realization; emitting the expansion "
                  "of already-given-up {2} at source=[{3},{4}) and refolding "
                  "the rest",
                  h.aStart, h.aEnd, DescribeOwner(*failingOwner),
                  *invocation->invB, *invocation->invE);
              continue;
            }
          }
        }
      }
    }

    // Use the explicit theorem-boundary interpretation for this last ownership
    // gap. By the time control reaches this branch, the engine has already
    // failed to prove a macro owner, include owner, truthful TU-owned byte
    // span, any exact/provable TU insertion anchor, and the declared
    // TUIncludeClosureEdit source-closure class. Do not manufacture a weaker
    // success class here; terminate via the named out-of-domain boundary
    // instead.
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::OwnerClosedCover,
            TerminalFallbackFailureReason::NoOwnerClosedCover,
            TerminalFallbackFailureContext::ForHunkTokenEnvelope(
                i, h.aStart, h.aEnd, h.bStart, h.bEnd)),
        "classify",
        ProofLattice().BuildOwnerUnresolvedNoTUAnchorDetail(i, h, tuPath, owner,
                                                            mapsToTU));
    continue;
  }

  return true;
}

std::optional<TextEdit> RefoldEngine::BuildDirectTUByteSpanEditForHunk(
    const diffutils::Hunk &h, size_t hunkIndex, bool isDel, StringRef tuPath,
    StringRef tuBytes, std::pair<uint64_t, uint64_t> span) {
  {
    {
      std::string repl;
      const uint64_t rawTUStart = span.first;
      const uint64_t rawTUEnd = span.second;
      std::optional<uint64_t> materializedBByteBegin;
      std::optional<uint64_t> materializedBByteEnd;
      if (auto bBytes =
              sourceMapper_.BTokenRangeToByteRange(h.bStart, h.bEnd)) {
        materializedBByteBegin = bBytes->first;
        materializedBByteEnd = bBytes->second;
      }
      if (isDel) {
        repl = "";
      } else {
        StringRef bSlice =
            h.isInsertOnly()
                ? refoldSliceTokenEnvelope(bTokOff_, bSource_, h.bStart, h.bEnd)
                : refoldSliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                                h.bStart, h.bEnd);
        repl.assign(bSlice.data(), bSlice.data() + bSlice.size());
        if (h.isInsertOnly()) {
          // For pure insertions the source slice is the *token envelope*, not
          // just the exact token byte cover.  Zero-normal-token sideband lines
          // can live between the inserted ordinary tokens and the next normal
          // token.  Use the actual envelope bytes when partitioning replay so a
          // separately materialized sideband replacement/deletion is not also
          // emitted by this ordinary TU insertion.
          std::optional<uint64_t> envelopeBegin;
          std::optional<uint64_t> envelopeEnd;
          if (!bSlice.empty()) {
            envelopeBegin =
                static_cast<uint64_t>(bSlice.data() - bSource_.data());
            envelopeEnd = *envelopeBegin + static_cast<uint64_t>(bSlice.size());
          }
          repl = textEditAssembler_->StripSeparatelyOwnedSidebandReplay(
              repl, envelopeBegin, envelopeEnd);
        }
      }

      // This patch inserts B text at a zero-width TU site: the TU span is
      // empty, but the hunk contributes one or more B tokens. Token-envelope
      // byte ranges begin at the first inserted token, so they do not include
      // any spaces or tabs that appear immediately before that token in B on
      // the same line. Preserve those preceding spaces/tabs when forming the
      // inserted text, unless equivalent spacing is already present immediately
      // to the left of the insertion point in the TU.
      if (!isDel && span.first == span.second && h.bStart < h.bEnd &&
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
              span.first > 0 && (tuBytes[span.first - 1] == ' ' ||
                                 tuBytes[span.first - 1] == '\t');
          if (!tuHasSpaceLeft) {
            repl.insert(0, std::string(bSource_.data() + p, b0 - p));
            materializedBByteBegin = static_cast<uint64_t>(p);
          }
        }
      }

      bool consumedSeparatorGapForPunctuation = false;

      if (!isDel && h.isInsertOnly()) {
        // B attaches the inserted content to its left neighbour iff there is no
        // whitespace before the first inserted B token.
        const size_t bStartIdx = static_cast<size_t>(h.bStart);
        const bool replayAttachesLeftInB =
            bStartIdx < bTokOff_.size() && bTokOff_[bStartIdx] > 0 &&
            !stringutils::isWs(bSource_[bTokOff_[bStartIdx] - 1]);
        consumedSeparatorGapForPunctuation =
            maybeConsumeLeftSourceGapWhenBAttaches(
                model_, pathIdentity_, tuPath, tuBytes, span, StringRef(repl),
                replayAttachesLeftInB, lexLang_);
      }

      TUEditPlanner().MaybeExtendTUSpanOverClosedTrailingCallSuffix(
          h, tuPath, tuBytes, repl, span);

      bool advancedOverSourceLineControlPrefix =
          maybeAdvanceTUInsertionPastSourceLineControlPrefix(
              TUEditPlanner(), lineControlProof_, h, tuPath, tuBytes, span);
      std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment;
      if (advancedOverSourceLineControlPrefix) {
        insertionAnchorAdjustment = TUInsertionAnchorAdjustment{
            TUInsertionAnchorAdjustmentKind::SourceLineControlPrefix,
            rawTUStart, span.first};
      }

      // If we are replacing whitespace-only text in the TU, we prefer to
      // preserve the existing TU gap whitespace rather than introducing new
      // whitespace from B.
      bool replacingGap = false;
      if (span.first < span.second) {
        std::string original(tuBytes.data() + span.first,
                             tuBytes.data() + span.second);
        replacingGap = !original.empty() && stringutils::isWs(original);
        if (replacingGap && !consumedSeparatorGapForPunctuation) {
          // Preserve exactly the gap as the replacement.
          repl = std::move(original);
        }
      } else if (span.second < span.first) {
        REFOLD_LOG_FATAL("tu/span", "invalid TU byte span: [{0},{1})",
                         span.first, span.second);
      }

      // If this patch replaces a non-empty whitespace gap in the TU, and the
      // replacement text does not already begin with whitespace, prefix a
      // single space so adjacent tokens remain separated. Add only one space,
      // even if the original gap was wider, to avoid duplicating spacing.
      if (replacingGap && !consumedSeparatorGapForPunctuation &&
          !repl.empty() && !stringutils::isWs(repl.front()))
        repl.insert(repl.begin(), ' ');

      // Keep a copy for logging; refoldPadAtBoundaries consumes via move.
      std::string rawRepl = repl;

      std::string padded = refoldPadAtBoundaries(
          tuBytes, static_cast<size_t>(span.first),
          static_cast<size_t>(span.second), std::move(repl),
          /*allowLeft*/ !replacingGap, /*allowRight*/ true, lexLang_);

      const bool skipLocalResync =
          tuInsertionBeforeMaterializedInclude(
              TUEditPlanner(), model_, sidebandPragmaEdits_, h, tuPath, span,
              /*requireVisibleReplayText=*/false) ||
          lineControlProof_.TUInsertionCanDeferResyncToConditionalJoin(
              advancedOverSourceLineControlPrefix, tuPath, span.second);

      ResyncOutcome ro =
          skipLocalResync
              ? ResyncOutcome(padded, std::nullopt)
              : textEditAssembler_->ApplyResyncOrPend(
                    tuBytes, span.first, span.second, padded, tuPath);
      return textEditAssembler_->BuildDirectTUHunkTextEdit(
          h, hunkIndex, span, std::move(ro), StringRef(padded), rawTUStart,
          rawTUEnd, materializedBByteBegin, materializedBByteEnd,
          AcceptedPathKind::TUByteSpanConservativeEdit,
          std::move(insertionAnchorAdjustment));
    }
  }
}

std::string RefoldEngine::FinalizeStructuralResult(
    StringRef tuPath, StringRef tuBytes, ArrayRef<diffutils::Hunk> hunks,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  std::vector<TextEdit> &tuEdits =
      structuralHunkDispatcher.MutableTUEditsForRepairAndEmission();
  // Global fail-closed composition rule: once this single structural pass has
  // requested terminal fallback, do not continue composing structural
  // artifacts. The outer driver will discard the current attempt and emit B
  // directly.
  if (terminalSink_.HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted after classification; "
                     "terminal fallback will be emitted");
    return std::string();
  }

  // Repair macro-state liveness after TU-level source edits.  The planner
  // owns the directive index, final TU edit interval queries, include ancestry
  // checks, proof witnesses, and conservative TU edit mutations used by all
  // macro-state repair phases.
  RefoldMacroStateRepairPlanner::MacroStateRepairRequest
      macroStateRepairRequest{tuPath, tuBytes, &structuralHunkDispatcher,
                              &tuEdits, &ownersMustExpand_};
  RefoldMacroStateRepairPlanner::MacroStateRepairPlan macroStateRepairPlan =
      MacroStateRepairPlanner().Plan(macroStateRepairRequest);
  if (!macroStateRepairPlan.success)
    return std::string();

  // Inject forced __COUNTER__ patches after normal hunk attribution.
  // This may create macro patches even when no diff hunk touched the invocation
  // (required to prevent later __COUNTER__ values from shifting after an edit).
  auto forcedCounters =
      CounterStabilization().ComputeForcedCounterPatches(tuPath, abTokMapA2B_);
  auto forcedCountersFromExpanded =
      CounterStabilization().ComputeForcedCounterPatchesFromExpandedMacros(
          tuPath, structuralHunkDispatcher.MacroPatchMergeBucketsForPlanner());
  if (!forcedCountersFromExpanded.empty())
    forcedCounters.append(forcedCountersFromExpanded.begin(),
                          forcedCountersFromExpanded.end());
  if (!forcedCounters.empty())
    applyForcedCounterPatches(forcedCounters, bToks_, sourceMapper_,
                              macroTopology_, MacroPatchPlanner(),
                              ProofLattice(), OwnerStateProof(),
                              structuralHunkDispatcher);

  auto sourceAuthoredLineControlDominatesSite =
      [&](const RefoldModel::MacroInvocation &site) -> bool {
    if (!site.invB || !site.invFile)
      return false;

    for (const RefoldModel::LineControlEvent &event :
         model_.GetLineControls()) {
      if (!event.active || !event.producerProven)
        continue;
      if (event.ownerIncludeId != site.ownerIncludeId)
        continue;
      if (!event.siteE || *event.siteE > *site.invB)
        continue;
      if (event.physicalFile.empty() || event.physicalFile.starts_with("<"))
        continue;
      if (!pathIdentity_.PathsEqual(event.physicalFile, *site.invFile))
        continue;

      // Only real source line controls discharge this source-preservation
      // case.  Built-in command-line line markers are recorded in the same
      // stream, but replaying them as source would not be a user-authored
      // repair dominating the surviving builtin spelling.
      StringRef text(event.text);
      text = text.ltrim();
      if (text.starts_with("#line") || (text.size() >= 2 && text[0] == '#' &&
                                        stringutils::isNonNewlineWs(text[1])))
        return true;
    }

    return false;
  };

  auto replayContextSensitivePredefinedBuiltin =
      [&](StringRef name, const RefoldModel::MacroInvocation &site) {
        // These builtins produce replay-time or physical-file-timestamp
        // literals that cannot be repaired by synthetic #line state.
        if (name == "__TIMESTAMP__" || name == "__DATE__" || name == "__TIME__")
          return true;

        if (name != "__BASE_FILE__")
          return false;

        // __BASE_FILE__ is normally part of the line/file observer model: a
        // preserved source-authored #line can make the spelling replay to the
        // same producer-observed value, and forcing B realization in those
        // cases destroys better source-preserving refoldings.  The remaining
        // unsafe case is a value-producing TU-local spelling that would
        // otherwise require a synthetic prologue solely to hide the checker
        // output path.  Prefer the local producer-proven literal for that
        // single observer instead of adding a global line directive at the top
        // of the refolded file.
        if (site.ownerIncludeId)
          return false;
        if (!site.invFile || !pathIdentity_.PathsEqual(*site.invFile, tuPath))
          return false;
        if (sourceAuthoredLineControlDominatesSite(site))
          return false;
        return true;
      };

  auto forceReplayContextSensitivePredefinedBuiltinPatches = [&]() {
    size_t forcedPredefinedObserverPatches = 0;

    for (const RefoldModel::MacroInvocation &builtin :
         model_.GetMacroInvocations()) {
      // Only value-producing builtin expansions need literalization here.
      // Builtins used as source #line operands normally have no preprocessed
      // token cover of their own; they are consumed while computing line/file
      // state.  Forcing a whole-cover patch for such zero-token operands either
      // has no valid B envelope or unnecessarily destroys the source #line
      // structure that already repairs downstream observers.
      if (!builtin.cover.IsValid())
        continue;
      if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(
              builtin))
        continue;

      const RefoldModel::MacroInvocation *site =
          lineControlProof_.LineStateObservableMacroSite(builtin);
      if (!site)
        continue;
      if (macroTopology_.IsInvocationInsideDefineDirective(*site))
        continue;
      if (!replayContextSensitivePredefinedBuiltin(builtin.name, *site))
        continue;

      // These predefined macros observe replay context that should not survive
      // as source spelling in the final output:
      //   * __TIMESTAMP__ names the timestamp of the physical file containing
      //     the spelling.
      //   * __DATE__ and __TIME__ name the time of the replay run itself.
      //   * A TU-local __BASE_FILE__ with no dominating source-authored #line
      //     would otherwise need a synthetic prologue to mask the checker
      //     output path.
      // Force a whole-cover macro realization so the emitted source carries the
      // producer-proven literal from B instead of depending on volatile replay
      // context or a gratuitous global line directive.
      const auto invStart = site->invB;
      const auto invEnd = site->invE;
      if (!invStart || !invEnd || *invEnd < *invStart)
        continue;

      std::optional<WholeCoverPlan> plan =
          MacroPatchPlanner().ComputeWholeCoverPlan(*site);
      if (!plan) {
        terminalSink_.RequestTerminalFallback(
            MakeTerminalFallbackProofFailure(
                TerminalFallbackObligationKind::ProducerFactsAvailable,
                TerminalFallbackFailureReason::MissingProducerFacts),
            "macro/predefined",
            llvm::formatv("replay-context-sensitive predefined macro '{0}' "
                          "invocation #{1} survived as source spelling, but "
                          "no whole-cover B realization plan is available",
                          builtin.name, site->id)
                .str());
        continue;
      }

      // Coalesce by physical invocation span, matching normal macro hunk
      // attribution.  Multiple volatile predefined builtins inside the same
      // enclosing macro callsite should force one deterministic whole-cover
      // realization for that callsite, not competing overlapping edits.
      RefoldStructuralHunkDispatcher::MacroPatchStagingSlot stagingSlot =
          structuralHunkDispatcher.PrepareMacroPatchStagingSlot(*site);

      MacroPatch patch{*invStart, *invEnd, plan->clippedText, site->id};
      if (stagingSlot.existingPatch)
        MacroPatchPlanner().CarryMacroPatchOwnerCertificate(
            patch, *stagingSlot.existingPatch);
      ProofLattice().CertifyMacroWholeCoverRealizationPatch(patch, *plan,
                                                            *site);
      MacroPatchPlanner().CertifyMacroPatchOwnerWitness(
          patch, site->ownerIncludeId ? Owner::Include(*site->ownerIncludeId)
                                      : Owner::TU());
      structuralHunkDispatcher.StageMacroPatch(stagingSlot, std::move(patch));
      ++forcedPredefinedObserverPatches;
    }

    if (forcedPredefinedObserverPatches != 0) {
      REFOLD_LOG_TRACE(
          "macro/predefined",
          "forced {0} replay-context-sensitive predefined macro invocation(s) "
          "to B-surface realization",
          forcedPredefinedObserverPatches);
    }
  };

  forceReplayContextSensitivePredefinedBuiltinPatches();

  // Materialize merged macro patches into the list buckets expected by later
  // planning passes. The dispatcher owns the DenseMap merge buckets and
  // flattens them deterministically before final include/TU emission.
  structuralHunkDispatcher.FinalizeMacroPatchBuckets(ProofLattice());

  if (!structuralHunkDispatcher.AppendLineObserverRealizationEdits(
          LineObserverLayout(), tuPath, tuBytes))
    return std::string();

  // Normalize/coalesce include-side insertions.
  structuralHunkDispatcher.OrderIncludeInsertions();

  // 5) Schedule include materializations.  The scheduler owns the run-local
  // child index, materialization seed set, realized expansion cache, and
  // source-graph/line-control metadata shared by the early materialization
  // include-materialization path and the later TU-root include edit emission
  // path.
  RefoldIncludeMaterializationScheduler::Dependencies includeSchedulerDeps;
  includeSchedulerDeps.model = &model_;
  includeSchedulerDeps.pathIdentity = &pathIdentity_;
  includeSchedulerDeps.includeMaterializer = includeMaterializer_.get();
  includeSchedulerDeps.includeInsertionPlanner = includeInsertionPlanner_.get();
  includeSchedulerDeps.lineObserverLayout = lineObserverLayout_.get();
  includeSchedulerDeps.macroStateRepairPlanner = macroStateRepairPlanner_.get();
  includeSchedulerDeps.textEditAssembler = textEditAssembler_.get();
  includeSchedulerDeps.proofLattice = proofLattice_.get();
  includeSchedulerDeps.pragmaOnceGuards = pragmaOnceGuardRewriter_.get();
  includeSchedulerDeps.terminalSink = &terminalSink_;
  includeSchedulerDeps.sidebandPragmaEdits = &sidebandPragmaEdits_;

  RefoldIncludeMaterializationScheduler::IncludeMaterializationRequest
      includeSchedulerRequest;
  includeSchedulerRequest.tuPath = tuPath;
  includeSchedulerRequest.tuBytes = tuBytes;
  includeSchedulerRequest.aSource = aSource_;
  includeSchedulerRequest.bSource = bSource_;
  includeSchedulerRequest.aTokens = aToks_;
  includeSchedulerRequest.bTokens = bToks_;
  includeSchedulerRequest.aTokenOffsets = aTokOff_;
  includeSchedulerRequest.bTokenOffsets = bTokOff_;
  includeSchedulerRequest.tokenHunks = hunks;
  includeSchedulerRequest.rawByteHunks =
      abByteHunks_ ? &*abByteHunks_ : nullptr;
  includeSchedulerRequest.structuralHunkDispatcher = &structuralHunkDispatcher;
  includeSchedulerRequest.ownersMustExpand = &ownersMustExpand_;

  RefoldIncludeMaterializationScheduler includeMaterializationScheduler(
      std::move(includeSchedulerDeps), std::move(includeSchedulerRequest));
  if (!includeMaterializationScheduler.MaterializeIncludeExpansions())
    return std::string();

  lastStats_.expandedIncludes =
      includeMaterializationScheduler.ExpandedIncludeIds().size();

  // 6) Final TU emission.  The emission planner owns TU macro edit
  // lowering, the second macro-state carry pass, TU-root include edit staging,
  // pending-resync-aware text assembly, final line-control prologue repair, and
  // final expanded-macro statistics.
  RefoldFinalTUEmissionPlanner::Dependencies finalEmissionDeps;
  finalEmissionDeps.model = &model_;
  finalEmissionDeps.macroTopology = &macroTopology_;
  finalEmissionDeps.lineControlProof = &lineControlProof_;
  finalEmissionDeps.lineDirs = &lineDirs_;
  finalEmissionDeps.macroStateRepairPlanner = macroStateRepairPlanner_.get();
  finalEmissionDeps.textEditAssembler = textEditAssembler_.get();
  finalEmissionDeps.proofLattice = proofLattice_.get();
  finalEmissionDeps.terminalSink = &terminalSink_;
  finalEmissionDeps.materializedEditMappings = materializedEditMappings_;
  finalEmissionDeps.finalLineControlPruneCandidates =
      &finalLineControlPruneCandidates_;
  finalEmissionDeps.finalLineControlSourceMappings =
      &finalLineControlSourceMappings_;

  RefoldFinalTUEmissionPlanner::EmissionRequest finalEmissionRequest;
  finalEmissionRequest.tuPath = tuPath;
  finalEmissionRequest.tuBytes = tuBytes;
  finalEmissionRequest.structuralHunkDispatcher = &structuralHunkDispatcher;
  finalEmissionRequest.includeMaterializationScheduler =
      &includeMaterializationScheduler;
  finalEmissionRequest.macroStatePlan = &macroStateRepairPlan;
  finalEmissionRequest.macroStateRequest = &macroStateRepairRequest;

  RefoldFinalTUEmissionPlanner finalEmissionPlanner(
      std::move(finalEmissionDeps));
  RefoldFinalTUEmissionPlanner::EmissionResult finalEmission =
      finalEmissionPlanner.PlanAndEmit(finalEmissionRequest);
  if (!finalEmission.success)
    return std::string();

  // Capture the exact final staged TU/include/macro topology after every
  // final-emission lowering and repair has been applied. This is evidence-only
  // and is consumed only by isolated alignment simulations.
  AlignmentSemanticTopologyKeyResult topologyKey =
      structuralHunkDispatcher.BuildAlignmentSemanticTopologyKey(
          ProofLattice().EquivalenceKeyBuilder(), tuBytes);
  for (uint64_t includeId :
       includeMaterializationScheduler.ExpandedIncludeIds())
    topologyKey.preservationFootprint.expandedIncludeIds.push_back(includeId);
  for (uint64_t macroRootId :
       structuralHunkDispatcher.ExpandedMacroRootIds())
    topologyKey.preservationFootprint.expandedMacroRootIds.push_back(
        macroRootId);
  llvm::sort(topologyKey.preservationFootprint.expandedIncludeIds);
  llvm::sort(topologyKey.preservationFootprint.expandedMacroRootIds);
  alignmentSimulationStagedTopologyComplete_ = topologyKey.complete;
  alignmentSimulationStagedTopologyFailure_ = std::move(topologyKey.failure);
  alignmentSimulationStagedTopologyKey_ = std::move(topologyKey.key);
  alignmentSimulationPreservationFootprint_ =
      std::move(topologyKey.preservationFootprint);

  lastStats_.expandedMacros = finalEmission.expandedMacroCount;
  return std::move(finalEmission.tuText);
}

bool RefoldEngine::AuditPreservedLineObserversInFinalOutput(
    StringRef finalSource) {
  // Without line directives the logical stream is not reconstructed at all, and
  // a terminal result carries no preserved observer to displace.
  if (!lineDirs_.Enabled() || terminalSink_.HasRequest())
    return true;

  // Alignment candidate simulations re-enter the engine.  Judging an assembly
  // that is about to be discarded would let a probe condemn the production
  // result, which is how two earlier attempts at this proof failed.
  if (alignmentSelectionOverride_)
    return true;

  // Enforce only the positions B produced by expanding `__LINE__`.  Those are
  // exactly the observers the deferral can displace, and restricting to them
  // keeps this from becoming a second, general re-check of the whole stream.
  //
  // A macro invocation's spans are recorded in the *original* stream's token
  // numbering, so they must be carried across the alignment before they can
  // index the edited stream.
  llvm::DenseSet<uint64_t> lineObserverBTokens;
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__LINE__")
      continue;
    for (const RefoldModel::PPSpan &span : macro.spans) {
      for (uint64_t aToken = span.begin; aToken < span.end; ++aToken) {
        if (aToken >= abTokMapA2B_.size())
          continue;
        const int64_t bToken = abTokMapA2B_[static_cast<size_t>(aToken)];
        if (bToken >= 0)
          lineObserverBTokens.insert(static_cast<uint64_t>(bToken));
      }
    }
  }
  if (lineObserverBTokens.empty())
    return true;

  const RefoldModel::PreprocessContext replayContext{
      model_.GetPPCwd().str(),
      std::vector<std::string>(model_.GetPPArgv().begin(),
                               model_.GetPPArgv().end()),
      model_.GetPPLang().str()};

  // Replay beside the producer's source, for the same reason the closing
  // assembly check does: an output directory may hold a later copy of a quoted
  // header, and resolving to it would audit observers the producer never saw.
  const std::optional<std::string> anchor =
      producerSourceAnchorPath(model_.GetSourcePath(), replayContext);
  if (!anchor) {
    REFOLD_LOG_DEBUG("line/observer-audit",
                     "skipped: no producer source anchor for replay");
    return true;
  }

  FinalSourcePreprocessCallback preprocess =
      buildFinalSourcePreprocessCallback(*anchor, replayContext,
                                         verifyIncludeDirs_);
  const std::optional<std::string> replayed = preprocess(finalSource);
  if (!replayed) {
    // The preprocessor could not be run.  That is a reason to skip the check,
    // never to reject an assembly this audit could not read.
    REFOLD_LOG_DEBUG("line/observer-audit",
                     "skipped: could not preprocess the assembled source");
    return true;
  }

  std::vector<PPTok> replayedToks;
  std::vector<size_t> replayedOff;
  lexPPTokens(*replayed, replayedToks, replayedOff,
              makeRefoldLexLangOptions(model_.GetPPLang()));

  const size_t common = std::min(replayedToks.size(), bToks_.size());
  for (size_t index = 0; index < common; ++index) {
    if (replayedToks[index].spelling == bToks_[index].spelling)
      continue;
    if (!lineObserverBTokens.count(static_cast<uint64_t>(index)))
      break; // A divergence elsewhere is out of this proof's scope.

    REFOLD_LOG_TRACE("line/observer-audit",
                     "displaced observer at B token {0}: assembled '{1}' vs "
                     "edited stream '{2}'",
                     static_cast<uint64_t>(index), replayedToks[index].spelling,
                     bToks_[index].spelling);
    terminalSink_.RequestTerminalFallback(
        RefoldOwnerStateProof::SuffixStabilityTerminalFailureForComponent(
            OwnerStateComponent::LineNumber),
        "line/observer-audit",
        llvm::formatv("assembled source re-expands a preserved __LINE__ "
                      "observer to '{0}' where the edited stream carries "
                      "'{1}' (B token {2})",
                      replayedToks[index].spelling, bToks_[index].spelling,
                      static_cast<uint64_t>(index))
            .str());
    return false;
  }

  // Report one audited position and the value it carries.  A silent no-op --
  // spans that never reach the edited stream's numbering, for instance -- still
  // reports a clean audit, so the count alone does not show the audit looked
  // anywhere meaningful.
  uint64_t firstObserver = std::numeric_limits<uint64_t>::max();
  for (uint64_t token : lineObserverBTokens)
    firstObserver = std::min(firstObserver, token);
  const StringRef firstSpelling =
      firstObserver < bToks_.size() ? StringRef(bToks_[firstObserver].spelling)
                                    : StringRef("<out-of-range>");
  REFOLD_LOG_TRACE("line/observer-audit",
                   "observers={0} tokens={1} firstObserver={2} carries='{3}' "
                   "clean",
                   static_cast<uint64_t>(lineObserverBTokens.size()),
                   static_cast<uint64_t>(common), firstObserver, firstSpelling);
  return true;
}

std::optional<uint64_t> RefoldEngine::FindSmallestOwnerForEditedToken(
    std::size_t editedTokenIndex) const {
  if (!finalAssemblyVerifier_)
    return std::nullopt;

  // The producer's maps index the edited stream as written.  Only a replay that
  // reproduces that stream token-for-token lets a verifier index be used as a
  // producer index; anything else would silently address the wrong token.
  const ArrayRef<PPTok> editedTokens = finalAssemblyVerifier_->EditedTokens();
  if (editedTokens.size() != bToks_.size())
    return std::nullopt;
  for (size_t index = 0; index < editedTokens.size(); ++index)
    if (editedTokens[index].spelling != bToks_[index].spelling)
      return std::nullopt;

  if (editedTokenIndex >= abTokMapB2A_.size())
    return std::nullopt;

  // The alignment relates tokens that agree across the two streams, so the
  // diverging token is by construction the one it cannot relate.  Anchor on the
  // nearest token it *can*: a neighbour inside the same expansion names the
  // same owner, and a neighbour outside it names an owner whose region still
  // contains the divergence once expanded.  Nearest-first with a left-hand tie
  // break keeps the choice deterministic.
  int64_t aToken = -1;
  for (size_t distance = 0;
       distance < abTokMapB2A_.size() && aToken < 0; ++distance) {
    if (editedTokenIndex >= distance) {
      const int64_t left = abTokMapB2A_[editedTokenIndex - distance];
      if (left >= 0) {
        aToken = left;
        break;
      }
    }
    const size_t right = editedTokenIndex + distance;
    if (right < abTokMapB2A_.size()) {
      const int64_t candidate = abTokMapB2A_[right];
      if (candidate >= 0)
        aToken = candidate;
    }
  }
  if (aToken < 0)
    return std::nullopt;

  return FindSmallestOwnerForAToken(static_cast<uint64_t>(aToken));
}

bool RefoldEngine::CertificationWindowCarriesAmbiguity(
    const std::pair<uint64_t, uint64_t> &aRange) const {
  // This is the resolver's own `WindowCarriesAmbiguity()` theorem, stated
  // against the forced map this attempt retained.  A forced anchor is an edge
  // every optimal path takes, so a window whose A tokens are all forced-matched
  // admits exactly one optimal map through it.  An A token the core theorem left
  // unmatched is the only way a competing optimal map can differ inside the
  // rectangle.
  const uint64_t aCount = static_cast<uint64_t>(alignmentForcedMap_.size());
  const uint64_t aEnd = std::min<uint64_t>(aRange.second, aCount);

  // A window reaching past the recorded forced map is not one this routine can
  // speak for; treat it as ambiguous rather than assume it is determined.
  if (aEnd != aRange.second)
    return true;

  for (uint64_t aToken = aRange.first; aToken < aEnd; ++aToken)
    if (alignmentForcedMap_[aToken] < 0)
      return true;
  return false;
}

bool RefoldEngine::AnyCertificationWindowCarriesAmbiguity() const {
  for (const std::pair<uint64_t, uint64_t> &range : certificationWindowARanges_)
    if (CertificationWindowCarriesAmbiguity(range))
      return true;
  return false;
}

bool RefoldEngine::AlignmentResolutionIsDemanded() const {
  // A candidate simulation is handed its alignment and must never ask for
  // another one; recursion is impossible by construction, and this keeps that
  // explicit at the demand boundary rather than relying on the resolver's own
  // guard.
  if (alignmentSelectionOverride_ || !alignmentSemanticResolverEnabled_)
    return false;

  if (terminalSink_.HasRequest()) {
    // The run gave up. Alignment is implicated only when a request localizes
    // its failure to A tokens: the resolver's whole lever is where anchors sit
    // inside a certification window, so a failure the plan could not tie to any
    // A token is not one a different anchor placement reaches. A materialized
    // include whose payload observes the very macro it consumes, for instance,
    // records the state component and no token range, and it fails identically
    // under every alignment -- proving that costs one complete refold of the
    // translation unit per enumerated candidate, and proves nothing.
    //
    // This is the same attribution discipline
    // `AppendNarrowableOwnersForTerminalRequests()` applies to regions, and it
    // is evidence rather than a cost model: a request that names tokens is
    // always honoured, however many candidates its window turns out to hold.
    for (const TerminalFallbackRequest &request : terminalSink_.Requests()) {
      const TerminalFallbackFailureContext &context = request.failure.context;
      // Either stream localizes the failure. A hunk that could not be realized
      // names the A tokens it consumed; a closing assembly check names the
      // edited token the two streams parted at. Both say the plan reached a
      // specific place and could not proceed there, which is what a different
      // anchor placement can change.
      if ((context.aTokenBegin && context.aTokenEnd) ||
          (context.bTokenBegin && context.bTokenEnd))
        return true;
    }
    return false;
  }

  // The run emitted source, so no ladder above would retry -- but emitting
  // source is not the same as refolding well. Expanding a macro root or
  // materializing an include is the concession the pipeline exists to avoid,
  // and forced-only anchors cause it directly: the hunk covering a callsite is
  // wider than the invocation, no carrier can preserve it, and the expansion
  // realization is taken. Resolving narrows the hunk and the callsite survives.
  //
  // A run that conceded nothing has nothing for resolution to restore, and that
  // is the common case -- which is what makes resolving nothing the default
  // rather than an optimization.
  if (!alignmentSimulationPreservationFootprint_.expandedIncludeIds.empty() ||
      !alignmentSimulationPreservationFootprint_.expandedMacroRootIds.empty())
    return true;

  // Preservation is not the only thing ambiguity decides. A hunk whose frontier
  // the core theorem did not pin had its placement chosen by tie-breaking
  // rather than proved, and resolution may put it somewhere else -- a pure
  // insertion landing on the far side of a conditional arm, say. The output is
  // sound either way, so no ladder above objects; it is simply not the
  // placement the theorems select.
  //
  // Ask whether the frontier can actually move, not whether ambiguity exists
  // somewhere nearby. A hunk sitting immediately between two forced anchors is
  // pinned by them: every optimal map takes those edges, so no realignment
  // reaches this hunk however ambiguous the rest of its window is. Asking the
  // weaker question -- does this hunk touch a window that carries ambiguity --
  // is true of nearly every real translation unit, and turns demand-driven
  // resolution back into eager resolution plus a wasted pass.
  for (const std::pair<uint64_t, uint64_t> &range : certificationWindowARanges_) {
    if (!CertificationWindowCarriesAmbiguity(range))
      continue;
    for (const diffutils::Hunk &hunk : abTokHunks_) {
      // Closed on both ends: a pure insertion occupies no token, so it sits
      // *at* a position, and a position on a window edge is reachable from
      // either side.
      if (hunk.aStart <= range.second && range.first <= hunk.aEnd)
        return true;
    }
  }
  return false;
}

std::optional<AlignmentSelectionOverride>
RefoldEngine::BuildLineAlignedHunkNarrowing(
    const diffutils::CertifiedLcsResult &coreAlignment) const {
  ArrayRef<int64_t> baseMap = coreAlignment.selectedMap;
  const auto &tokmapByPP = model_.GetTokmapByPP();
  StringRef tuPath = model_.GetSourcePath();
  StringRef tuBytes = tuSourceBytes_;

  // Source extent of one A-token run, when every token is spelled in the TU.
  auto runSourceRange =
      [&](uint64_t aBegin,
          uint64_t aEnd) -> std::optional<std::pair<uint64_t, uint64_t>> {
    uint64_t begin = std::numeric_limits<uint64_t>::max();
    uint64_t end = 0;
    for (uint64_t aToken = aBegin; aToken < aEnd; ++aToken) {
      auto entry = tokmapByPP.find(aToken);
      if (entry == tokmapByPP.end() ||
          !pathIdentity_.PathsEqual(entry->second.file, tuPath))
        return std::nullopt;
      begin = std::min(begin, entry->second.b);
      end = std::max(end, entry->second.e);
    }
    if (begin > end || end > tuBytes.size())
      return std::nullopt;
    return std::make_pair(begin, end);
  };

  auto aTokenSpellingMatchesB = [&](uint64_t aToken, uint64_t bToken) {
    return aToken < aToks_.size() && bToken < bToks_.size() &&
           aToks_[aToken].spelling == bToks_[bToken].spelling;
  };

  auto removesNoNewline = [&](std::pair<uint64_t, uint64_t> range) {
    return tuBytes.slice(range.first, range.second).find('\n') ==
           StringRef::npos;
  };

  for (const TerminalFallbackRequest &request : terminalSink_.Requests()) {
    const TerminalFallbackFailureContext &context = request.failure.context;
    if (!context.aTokenBegin || !context.aTokenEnd || !context.bTokenBegin ||
        !context.bTokenEnd)
      continue;
    const uint64_t aBegin = *context.aTokenBegin;
    const uint64_t aEnd = *context.aTokenEnd;
    const uint64_t bBegin = *context.bTokenBegin;
    const uint64_t bEnd = *context.bTokenEnd;
    // Only a replacement narrows; a pure deletion has no payload to re-anchor.
    if (bEnd <= bBegin || aEnd <= aBegin || aEnd > baseMap.size())
      continue;
    // The whole A interval must be unmatched, or the payload is already
    // anchored and this is not the suppressed-anchor shape.
    bool wholeIntervalUnmatched = true;
    for (uint64_t aToken = aBegin; aToken < aEnd && wholeIntervalUnmatched;
         ++aToken)
      wholeIntervalUnmatched = baseMap[aToken] < 0;
    if (!wholeIntervalUnmatched)
      continue;

    const uint64_t payload = bEnd - bBegin;
    if (payload >= aEnd - aBegin)
      continue;

    std::optional<AlignmentSelectionOverride> sole;
    uint64_t qualifying = 0;
    for (uint64_t leftPeel = 0; leftPeel <= payload; ++leftPeel) {
      const uint64_t rightPeel = payload - leftPeel;
      // Every peeled pair must agree in spelling, or the map would stop
      // agreeing with the streams.
      bool admissible = true;
      for (uint64_t i = 0; i < leftPeel && admissible; ++i)
        admissible = aTokenSpellingMatchesB(aBegin + i, bBegin + i);
      for (uint64_t j = 0; j < rightPeel && admissible; ++j)
        admissible = aTokenSpellingMatchesB(aEnd - 1 - j, bEnd - 1 - j);
      if (!admissible)
        continue;

      const uint64_t runBegin = aBegin + leftPeel;
      const uint64_t runEnd = aEnd - rightPeel;
      if (runEnd <= runBegin)
        continue;
      std::optional<std::pair<uint64_t, uint64_t>> range =
          runSourceRange(runBegin, runEnd);
      if (!range || !removesNoNewline(*range))
        continue;

      ++qualifying;
      if (qualifying > 1)
        break;

      AlignmentSelectionOverride selection;
      selection.selectedMap.assign(baseMap.begin(), baseMap.end());
      for (uint64_t i = 0; i < leftPeel; ++i)
        selection.selectedMap[aBegin + i] = static_cast<int64_t>(bBegin + i);
      for (uint64_t j = 0; j < rightPeel; ++j)
        selection.selectedMap[aEnd - 1 - j] = static_cast<int64_t>(bEnd - 1 - j);
      selection.selectedAnchorProofs.assign(selection.selectedMap.size(),
                                            diffutils::LcsAnchorProof{});
      selection.globalObjective = coreAlignment.globalObjective;
      selection.globalObjectiveIsExact = coreAlignment.globalObjectiveIsExact;
      selection.allWindowsCertified = coreAlignment.allWindowsCertified;
      selection.certifiedBoundaries = coreAlignment.certifiedBoundaries;
      selection.certificationWindows = coreAlignment.certificationWindows;
      for (size_t aToken = 0; aToken < selection.selectedMap.size(); ++aToken) {
        if (selection.selectedMap[aToken] < 0)
          continue;
        if (aToken < coreAlignment.selectedAnchorProofs.size() &&
            coreAlignment.selectedAnchorProofs[aToken].kind ==
                diffutils::LcsAnchorProofKind::CoreOptimalPathForced) {
          selection.selectedAnchorProofs[aToken] =
              coreAlignment.selectedAnchorProofs[aToken];
          continue;
        }
        selection.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
            diffutils::LcsAnchorProofKind::OwnerAlignedDeletionSlide, 0};
      }
      REFOLD_LOG_INFO(
          "fallback",
          "terminal fallback names replacement hunk A=[{0},{1}) B=[{2},{3}); "
          "re-anchoring {4} payload token(s) leaves the sole deletion "
          "A=[{5},{6}) source=[{7},{8}) that removes no source line",
          aBegin, aEnd, bBegin, bEnd, payload, runBegin, runEnd, range->first,
          range->second);
      sole = std::move(selection);
    }
    if (qualifying == 1 && sole)
      return sole;
  }
  return std::nullopt;
}

bool RefoldEngine::AppendNarrowableOwnersForTerminalRequests(
    const llvm::DenseSet<uint64_t> &alreadyExpanded,
    llvm::SmallVectorImpl<uint64_t> &owners) const {
  // Every region the ledger names is independently at fault, so give up all of
  // them in one attempt.  Taking only the first would cost one re-assembly per
  // region and, past the attempt ceiling, would reach the terminal carrier with
  // narrowing still available.  A request naming nothing is not a defect: a
  // translation-unit wide producer inconsistency has no smaller region to name.
  bool everyRequestNarrowable = true;
  for (const TerminalFallbackRequest &request : terminalSink_.Requests()) {
    const TerminalFallbackFailureContext &context = request.failure.context;

    // A request that names its region explicitly names exactly one, and it is
    // the region whose realization failed.  A request that instead recorded the
    // A tokens it could not realize may span several, so take every one of them
    // -- expanding only the region owning the first token cannot repair a
    // divergence sitting inside a later one.
    llvm::SmallVector<uint64_t, 8> requestOwners;
    if (context.ownerId)
      requestOwners.push_back(*context.ownerId);
    else if (context.aTokenBegin && context.aTokenEnd)
      AppendMinimalOwnersCoveringATokenRange(*context.aTokenBegin,
                                             *context.aTokenEnd, requestOwners);

    if (requestOwners.empty()) {
      everyRequestNarrowable = false;
      continue;
    }

    // Widen past anything already given up, exactly as the verification path
    // does: expanding it once was not enough.  Each region widens on its own --
    // they can sit at different depths, and one already-expanded region must
    // not suppress a sibling that has never been given up.
    bool namedAnyRegion = false;
    for (uint64_t requestOwner : requestOwners) {
      std::optional<uint64_t> owner = requestOwner;
      while (owner && alreadyExpanded.count(*owner))
        owner = FindEnclosingOwner(*owner);
      if (!owner)
        continue;
      namedAnyRegion = true;
      if (!llvm::is_contained(owners, *owner))
        owners.push_back(*owner);
    }
    if (!namedAnyRegion)
      everyRequestNarrowable = false;
  }
  return everyRequestNarrowable;
}

std::optional<uint64_t>
RefoldEngine::FindSmallestOwnerForAToken(uint64_t aToken) const {
  // Several nested owners can produce the same A token: an invocation inside an
  // invocation, either of them inside an include.  Take the narrowest, because
  // expanding it gives up the least source structure; the caller widens outward
  // only if that proves insufficient.  Width ties are broken by producer id so
  // the choice does not depend on model order.
  //
  // Macro invocations and include instances share one producer id space, so the
  // result names an owner without having to say which kind it is.  That is what
  // lets the caller record it in a single set and leave each subsystem's own
  // expansion path to act on it.
  std::optional<uint64_t> ownerId;
  uint64_t ownerWidth = std::numeric_limits<uint64_t>::max();
  auto considerOwner = [&](uint64_t id, uint64_t begin, uint64_t end) {
    if (aToken < begin || aToken >= end)
      return;
    const uint64_t width = end - begin;
    if (width < ownerWidth || (width == ownerWidth && ownerId && id < *ownerId)) {
      ownerId = id;
      ownerWidth = width;
    }
  };

  for (const RefoldModel::MacroInvocation &macro : model_.GetMacroInvocations())
    for (const RefoldModel::PPSpan &span : macro.spans)
      considerOwner(macro.id, span.begin, span.end);

  // An include's cover is its whole expansion, so it is wider than any macro
  // inside it and wins only when no invocation covers the token at all -- a
  // location observer in a header body, for instance.  Before this, such a
  // divergence had no owner and went straight to whole-translation-unit
  // expansion.
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes())
    if (include.cover.IsValid())
      considerOwner(include.id, include.cover.begin, include.cover.end);

  return ownerId;
}

void RefoldEngine::AppendMinimalOwnersCoveringATokenRange(
    uint64_t aBegin, uint64_t aEnd,
    llvm::SmallVectorImpl<uint64_t> &owners) const {
  if (aEnd <= aBegin)
    return;

  // Collect the regions that meet the range at all before asking about
  // individual tokens.  The model is walked once here rather than once per
  // token, which keeps a wide failing range from costing a full scan of every
  // invocation and include for each of its tokens.
  struct CoveringOwner {
    uint64_t id;
    uint64_t begin;
    uint64_t end;
  };
  SmallVector<CoveringOwner, 16> covering;
  auto considerOwner = [&](uint64_t id, uint64_t begin, uint64_t end) {
    if (end <= aBegin || begin >= aEnd)
      return;
    covering.push_back(CoveringOwner{id, begin, end});
  };

  for (const RefoldModel::MacroInvocation &macro : model_.GetMacroInvocations())
    for (const RefoldModel::PPSpan &span : macro.spans)
      considerOwner(macro.id, span.begin, span.end);
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes())
    if (include.cover.IsValid())
      considerOwner(include.id, include.cover.begin, include.cover.end);

  // Each token contributes the narrowest region covering it, with width ties
  // broken by producer id so the choice does not depend on model order.  A
  // token no region covers is owned by the translation unit and contributes
  // nothing, which is what leaves a wholly TU-owned range unattributed.
  SmallVector<uint64_t, 8> selected;
  for (uint64_t aToken = aBegin; aToken < aEnd; ++aToken) {
    std::optional<uint64_t> narrowestId;
    uint64_t narrowestWidth = std::numeric_limits<uint64_t>::max();
    for (const CoveringOwner &candidate : covering) {
      if (aToken < candidate.begin || aToken >= candidate.end)
        continue;
      const uint64_t width = candidate.end - candidate.begin;
      if (width < narrowestWidth ||
          (width == narrowestWidth && narrowestId && candidate.id < *narrowestId)) {
        narrowestId = candidate.id;
        narrowestWidth = width;
      }
    }
    if (narrowestId && !llvm::is_contained(selected, *narrowestId))
      selected.push_back(*narrowestId);
  }

  llvm::sort(selected);
  for (uint64_t ownerId : selected)
    if (!llvm::is_contained(owners, ownerId))
      owners.push_back(ownerId);
}

std::string RefoldEngine::DescribeOwner(uint64_t ownerId) const {
  if (const RefoldModel::MacroInvocation *macro =
          macroTopology_.FindMacroInvocationById(ownerId))
    return ("macro invocation " + Twine(ownerId) + " (" + macro->name + ")")
        .str();
  if (const RefoldModel::IncludeItem *include = model_.GetIncludeById(ownerId))
    return ("include " + Twine(ownerId) + " (" + include->target + ")").str();
  return ("region " + Twine(ownerId)).str();
}

std::optional<uint64_t>
RefoldEngine::FindEnclosingOwner(uint64_t ownerId) const {
  // Widening follows producer ancestry, never source overlap.  An invocation
  // widens to its caller and then to the include instance that owns its
  // callsite; an include widens to the include that entered it.  Running out of
  // ancestry means the translation unit is the only region left.
  if (const RefoldModel::MacroInvocation *macro =
          macroTopology_.FindMacroInvocationById(ownerId)) {
    if (macro->callerMacroId)
      return macro->callerMacroId;
    return macro->ownerIncludeId;
  }
  if (const RefoldModel::IncludeItem *include = model_.GetIncludeById(ownerId))
    return include->parent;
  return std::nullopt;
}

std::string RefoldEngine::RunRefoldPass() {
  if (!ValidateTokenCount())
    return std::string();

  StringRef tuPath = model_.GetSourcePath();
  REFOLD_LOG_INFO(
      "plan",
      "starting refold: tu={0} ppBytes={1} ppModBytes={2} ppTokens={3} "
      "ppModTokens={4}",
      tuPath, aSource_.size(), bSource_.size(), aToks_.size(), bToks_.size());

  // Reset the structural planning phase and run-local token diff caches before
  // any stage repopulates them for this pass.  PlanTokenDiff() advances the
  // phase monotonically through initial diff, tiling, and insertion-ledger
  // publication before DispatchStructuralHunks() is allowed to run.
  structuralHunkPlanningPhase_ = StructuralHunkPlanningPhase::NotStarted;
  abTokHunks_.clear();
  abTokMapA2B_.clear();
  abTokMapB2A_.clear();
  abTokAnchorProofs_.clear();
  alignmentSemanticResolutionWitnesses_.clear();
  alignmentSimulationStagedTopologyKey_.clear();
  alignmentSimulationStagedTopologyComplete_ = true;
  alignmentSimulationStagedTopologyFailure_.clear();
  alignmentSimulationPreservationFootprint_ =
      AlignmentSemanticPreservationFootprint{};
  alignmentSemanticTheoremActive_ = alignmentSelectionOverride_.has_value();

  std::unique_ptr<llvm::MemoryBuffer> tuBuffer = LoadTUSource(tuPath);
  StringRef tuBytes = tuBuffer->getBuffer();
  std::vector<diffutils::Hunk> hunks = PlanTokenDiff(tuPath);

  // A probe has what it came for: token-diff planning is where alignment
  // resolution publishes its theorem.  Planning past this point would be the
  // very pass the probe exists to decide against running.
  if (stopAfterAlignmentResolution_)
    return std::string();

  TraceStructuralHunkEnvelopes(hunks);

  RefoldStructuralHunkDispatcher structuralHunkDispatcher;
  if (!StageSidebandEdits(tuPath, tuBytes, structuralHunkDispatcher))
    return std::string();
  if (!DispatchStructuralHunks(tuPath, tuBytes, hunks,
                                structuralHunkDispatcher))
    return std::string();
  return FinalizeStructuralResult(tuPath, tuBytes, hunks,
                                  structuralHunkDispatcher);
}

} // namespace refold
} // namespace clang
