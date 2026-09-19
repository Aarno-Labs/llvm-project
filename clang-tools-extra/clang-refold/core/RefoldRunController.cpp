//===--- RefoldRunController.cpp --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The run controller: `refoldTranslationUnit()` refolds one translation unit
// by running as many complete `RefoldEngine` passes as its retry ladders need.
//
// Each pass is a fresh engine fixed at construction by a `RefoldPassConfig`.
// Between passes the controller resolves alignment ambiguity on demand,
// repairs an owner-straddling deletion run, and narrows the regions a terminal
// request or the closing assembly check names.  It reads a finished pass only
// through the engine's public post-pass queries.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldEngine.h"

#include "proof/RefoldOwnerStateProof.h"
#include "source/RefoldTokenDiffPlanner.h"
#include "support/RefoldLog.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

/// Describe which attribution surfaces one terminal request carries.
///
/// A region-scoped realization needs somewhere to put the payload it cannot
/// prove, and the fields the narrowing ladder reads are `ownerId`, the A token
/// range, and -- for a check that runs against the assembled source and knows
/// only where the two streams parted -- the B token range.  Reporting the whole
/// surface, the hunk and the source span included, is what turns "does every
/// request name a region" into a question with an observable answer rather than
/// an audit.
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
  auto spellingsEqual = [&](uint64_t lhs, uint64_t rhs) {
    return lhs < aToks.size() && rhs < aToks.size() &&
           aToks[lhs].spelling == aToks[rhs].spelling;
  };

  // Return how far the run `[aBegin, aEnd)` may slide in one direction.
  //
  // A slide of distance d is admissible exactly when each of its d steps
  // exchanges two identically spelled tokens, so admissibility is prefix
  // monotone: distance d requires everything distance d-1 requires, plus one
  // further exchange. The admissible distances are therefore a contiguous
  // prefix, and its length is the extent of the adjacent run of
  // period-`(aEnd - aBegin)` spelling repetition, bounded by the end of the
  // stream. That is the exact finite bound the streams themselves impose;
  // walking outward until the first exchange fails computes it directly, so no
  // fixed search window is needed. None would be sound to impose either: an
  // owner-closing slide one position past an arbitrary cutoff is exactly as
  // admissible as one inside it, and refusing it would fail a run whose repair
  // the streams prove.
  auto maxAdmissibleSlideDistance = [&](uint64_t aBegin, uint64_t aEnd,
                                        bool left) {
    uint64_t distance = 0;
    while (true) {
      const uint64_t next = distance + 1;
      // The step must stay inside the stream: sliding left past its start or
      // right past its end has no token to exchange.
      if (left ? aBegin < next : aEnd + next > baseMap.size())
        break;
      const bool exchangeable = left
                                    ? spellingsEqual(aBegin - next, aEnd - next)
                                    : spellingsEqual(aBegin + distance,
                                                     aEnd + distance);
      if (!exchangeable)
        break;
      distance = next;
    }
    return distance;
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
    const uint64_t maxLeft =
        maxAdmissibleSlideDistance(aBegin, aEnd, /*left=*/true);
    const uint64_t maxRight =
        maxAdmissibleSlideDistance(aBegin, aEnd, /*left=*/false);
    const uint64_t maxDistance = maxLeft > maxRight ? maxLeft : maxRight;

    // Nearest admissible position first, so a run is nudged the shortest
    // distance that reaches a cover.
    for (uint64_t distance = 1; distance <= maxDistance; ++distance) {
      // Left first, then right, so the choice does not depend on iteration
      // order anywhere else.
      for (int direction : {-1, 1}) {
        const bool left = direction < 0;
        // Past this direction's admissible prefix the exchange fails; the
        // other direction may still reach further.
        if (distance > (left ? maxLeft : maxRight))
          continue;

        const uint64_t movedBegin =
            left ? aBegin - distance : aBegin + distance;
        const uint64_t movedEnd = movedBegin + runLength;

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
  return createStringError(
      std::make_error_code(std::errc::illegal_byte_sequence),
      "no admissible refold: %llu terminal request(s) reached the seam and "
      "none could be narrowed; emitting the edited preprocessed stream would "
      "drop every comment and directive in the translation unit. See the "
      "fallback/carrier census above for the failing obligations",
      static_cast<unsigned long long>(requests.size()));
}

Expected<std::string> refoldTranslationUnit(
    const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict,
    ProofAuditMode proofAuditMode, StringRef finalOutputPath,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings,
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

  // The raw byte diff is likewise one answer per run.  See `RawByteHunkMemo`:
  // it reads the A and B buffers alone, and an attempt changes neither, so an
  // attempt, a candidate simulation, and the resolution probe all build the
  // identical hunks from the identical bytes.
  RawByteHunkMemo rawByteHunkMemo;

  // The owner-state census is likewise one answer per run.  See
  // `OwnerStateGraphMemo`: it reads the producer model and the A stream alone,
  // and every attempt clones that model from this one parse, so an attempt, a
  // candidate simulation, and the resolution probe all census identical owners.
  OwnerStateGraphMemo ownerStateGraphMemo;

  // Set once, by the owner-alignment repair below, and then carried by every
  // later attempt: the repaired alignment is the one the run planned from from
  // that point on.
  std::optional<AlignmentSelectionOverride> ownerAlignedSlideOverride;
  bool attemptedOwnerAlignedSlide = false;

  for (unsigned attempt = 0;; ++attempt) {
    RefoldPassConfig config;
    config.role = ("production attempt " + Twine(attempt)).str();
    config.noLines = noLines;
    config.strict = strict;
    config.proofAuditMode = proofAuditMode;
    config.finalOutputPath = finalOutputPath;
    config.sidebandPragmaEdits = sidebandPragmaEdits;
    config.sidebandPragmaLinePairings = sidebandPragmaLinePairings;
    config.materializedEditMappings = materializedEditMappings;
    // Copied, not moved: this runs once per attempt, and a moved-from callback
    // would silently disable final line-control validation for every attempt
    // after the first.
    config.finalLineControlValidationCallback =
        finalLineControlValidationCallback;
    config.finalAssemblyVerifier = assemblyVerifier;
    config.verifyIncludeDirs = verifyIncludeDirs;
    config.ownersMustExpand = ownersMustExpand;
    config.resolveAlignmentAmbiguity = resolveAlignmentAmbiguity;
    config.alignmentSelectionOverride = ownerAlignedSlideOverride;
    config.alignmentResolutionMemo = &alignmentResolutionMemo;
    config.alignmentCertificationMemo = &alignmentCertificationMemo;
    config.rawByteHunkMemo = &rawByteHunkMemo;
    config.ownerStateGraphMemo = &ownerStateGraphMemo;
    // Each attempt needs its own model.  Moving the parsed one in would leave
    // every later attempt building an engine from a moved-from model -- no
    // includes, no macro invocations, no directives -- so a marked region would
    // have nothing to be marked against and the ladder would appear to make no
    // progress.  `CloneForReadOnlyConsumer` exists for this: a plain copy would
    // leave the clone's lookup tables pointing into the original's storage.
    RefoldEngine engine(mOrErr->CloneForReadOnlyConsumer(), aSource, aToks,
                        aTokOff, bSource, bToks, bTokOff, std::move(config));

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
        // Say what the run is about to spend before it spends it.  Everything
        // between here and the probe's verdict is one complete replan of this
        // translation unit followed by one more per enumerated candidate
        // alignment map, each logging the same per-pass lines as the attempt
        // above -- so without this the log reads as a loop repeating itself
        // rather than as distinct alignments being realized and compared.
        uint64_t ambiguousWindows = 0;
        for (const std::pair<uint64_t, uint64_t> &range :
             engine.CertificationWindowARanges())
          if (engine.CertificationWindowCarriesAmbiguity(range))
            ++ambiguousWindows;
        REFOLD_LOG_INFO(
            "fallback",
            "attempt {0} is limited by alignment ambiguity, and {1} of {2} "
            "certified window(s) carry it; probing resolution, which replans "
            "this translation unit once and then realizes each window's "
            "enumerated candidate maps as a complete refold apiece",
            attempt, ambiguousWindows,
            static_cast<uint64_t>(engine.CertificationWindowARanges().size()));

        // The probe must present the same optional output surfaces as the
        // attempt it stands in for, because a candidate simulation mirrors its
        // parent's surfaces and two alignments may otherwise be separated by
        // provenance production was never asked for.  Only whether the sidecar
        // exists is observable, so scratch storage answers it without
        // disturbing the mappings that belong to the attempt that will be
        // emitted.
        std::vector<MaterializedEditMapping> probeMaterializedEditMappings;
        RefoldPassConfig probeConfig;
        probeConfig.role = "alignment resolution probe";
        probeConfig.noLines = noLines;
        probeConfig.strict = strict;
        probeConfig.proofAuditMode = proofAuditMode;
        probeConfig.finalOutputPath = finalOutputPath;
        probeConfig.sidebandPragmaEdits = sidebandPragmaEdits;
        probeConfig.sidebandPragmaLinePairings = sidebandPragmaLinePairings;
        probeConfig.materializedEditMappings =
            materializedEditMappings ? &probeMaterializedEditMappings : nullptr;
        // A probe stops before final line-control pruning, the callback's only
        // consumer, and a candidate simulation is handed an empty one
        // regardless -- so resolution cannot observe its absence.  The probe
        // is not judged either, so it carries no assembly verifier.
        probeConfig.verifyIncludeDirs = verifyIncludeDirs;
        probeConfig.ownersMustExpand = ownersMustExpand;
        probeConfig.resolveAlignmentAmbiguity = true;
        probeConfig.alignmentResolutionMemo = &alignmentResolutionMemo;
        // The probe certifies nothing new: this attempt already recorded the
        // alignment, so the probe replays it and spends its time only on the
        // resolution the next attempt would otherwise have paid for.
        probeConfig.alignmentCertificationMemo = &alignmentCertificationMemo;
        // The probe reaches the end of token-diff planning, which is where the
        // raw byte diff is built, so without this it would rebuild the hunks
        // the attempt it stands in for has already published.
        probeConfig.rawByteHunkMemo = &rawByteHunkMemo;
        // The probe drives the same candidate simulations the resolver would,
        // and each of those censuses owners, so without this the probe and its
        // candidates would rebuild the census this attempt already published.
        probeConfig.ownerStateGraphMemo = &ownerStateGraphMemo;
        RefoldEngine probe(mOrErr->CloneForReadOnlyConsumer(), aSource, aToks,
                           aTokOff, bSource, bToks, bTokOff,
                           std::move(probeConfig));
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
    if (engine.TerminalSink().HasRequest()) {
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
                    engine.TerminalSink().Requests())) {
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
      const RefoldEngine::TerminalRequestNarrowingCensus census =
          engine.AppendNarrowableOwnersForTerminalRequests(ownersMustExpand,
                                                           owners);
      // A request that names a region is not the same as a request the ladder
      // can act on: the region may already be given up with nothing enclosing
      // it.  Say which, so the census `takeTerminalCarrier()` prints -- which
      // reports whether a request names a region at all -- does not read as a
      // contradiction of this line.
      if (!census.EveryRequestNarrowable()) {
        if (owners.empty())
          REFOLD_LOG_INFO(
              "fallback",
              "no terminal request names a region the ladder can still give "
              "up: requests={0} namingNoRegion={1} "
              "namingOnlyGivenUpRegions={2}; taking the carrier",
              census.requests, census.unattributed, census.exhausted);
        else
          REFOLD_LOG_INFO(
              "fallback",
              "{0} of {1} terminal request(s) name no region the ladder can "
              "still give up: namingNoRegion={2} namingOnlyGivenUpRegions={3}; "
              "narrowing the {4} region(s) the others named and re-planning",
              census.unattributed + census.exhausted, census.requests,
              census.unattributed, census.exhausted,
              static_cast<uint64_t>(owners.size()));
      }
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
      return takeTerminalCarrier(std::move(out),
                                 engine.TerminalSink().Requests());
    }

    // Without a verifier the result stands exactly as it would without this
    // loop.
    if (!assemblyVerifier)
      return out;

    const FinalAssemblyVerdict verdict = assemblyVerifier->Verify(out);
    // Only a proven divergence names a region to narrow, so only `Diverged`
    // reaches the repair ladder below.
    if (verdict.kind != FinalAssemblyVerdictKind::Diverged) {
      if (verdict.kind == FinalAssemblyVerdictKind::Inconclusive) {
        // `fatal` asks for an assembly the closing check has verified, and a
        // comparison that could not be performed does not produce one.  "I
        // could not check" is not "it checked out", so failing is the only
        // answer that reports what actually happened; shipping here would
        // hand back a result whose verification never ran under the very
        // option asking for it.  This is the option's contract, not a
        // soundness claim: the check is defense in depth, and its absence
        // leaves the proof paths exactly as they would be with the check off.
        if (DispositionForVerdict(verifyMode, verdict.kind) ==
            FinalAssemblyDisposition::Fail)
          return createStringError(
              std::make_error_code(std::errc::illegal_byte_sequence),
              "refolded source could not be preprocessed for verification, so "
              "--verify-output=fatal has nothing to compare it against and "
              "cannot report it as verified. Re-run with "
              "--verify-output=repair to take the unchecked result, or with "
              "--verify-output=off to skip the check");

        // `repair` keeps it.  An inconclusive verdict names no diverging
        // region, so there is nothing for the ladder to expand, and rejecting
        // it would trade a real refold for a missing measurement rather than
        // for a proven defect.  Say so where a run that asked for
        // verification will see it: at debug level the one signal that the
        // check did not happen is indistinguishable from the check passing.
        REFOLD_LOG_WARN("assembly-verify",
                        "assembly NOT verified: the final source could not be "
                        "preprocessed for checking, so --verify-output has "
                        "nothing to compare and the result stands unchecked");
      } else if (attempt > 0) {
        REFOLD_LOG_INFO("assembly-verify",
                        "verified after {0} narrowing step(s)", attempt);
      }
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
    if (DispositionForVerdict(verifyMode, verdict.kind) ==
        FinalAssemblyDisposition::Fail) {
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
    engine.RecordUnnarrowableDivergence(verdict.mismatchTokenIndex);
    // The census is the request ledger as of the request recorded just above,
    // which is why it is taken after that request.  It is copied before the
    // carrier is built, because building the carrier may append a
    // terminal-audit request and reallocate the ledger under a borrowed view.
    const std::vector<TerminalFallbackRequest> census(
        engine.TerminalSink().Requests().begin(),
        engine.TerminalSink().Requests().end());
    return takeTerminalCarrier(engine.ResolvePostStructuralFallback(), census);
  }
}

} // namespace refold
} // namespace clang
