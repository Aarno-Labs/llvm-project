//===--- RefoldExpansionFallbackPlanner.cpp ---------------------*- C++ -*-===//
//
// Expansion fallback planning and TU include-closure realization.
//
// RefoldExpansionFallbackPlanner owns the deterministic fallback planning
// surface used after ordinary TU/include/macro structural emission has failed
// closed.  It receives explicit run state, proof services, mutable ledgers, and
// orchestration callbacks.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldExpansionFallbackPlanner.h"

#include "core/RefoldLog.h"
#include "edit/RefoldSourceEnvelopeTiling.h"
#include "include/IncludeSpellingHelpers.h"
#include "include/RefoldIncludeInsertionPlanner.h"
#include "include/RefoldIncludeMaterializer.h"
#include "include/RefoldIncludeReplayProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "line-control/SourceLineDirectiveHelpers.h"
#include "macro/RefoldMacroStateProof.h"
#include "proof/RefoldNeutralityProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldPragmaTaxonomy.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldSourceGapProof.h"
#include "source/RefoldTokenlessSourceProjection.h"
#include "source/TokenTextHelpers.h"
#include "util/RefoldDenseMapInfo.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

// This translation unit contains the closure/materialization fallback boundary.
// The terminal resolver normalizes a failed structural run onto the declared
// raw-B terminal carrier; it does not run an additional proof-bypass expansion
// search.  Source-line directive parsing/rewrite helpers live in
// RefoldLineControlRewrite.cpp and are exposed through the private helper
// header.

// Post-terminal macro expansion is classified through terminal carriers.
//
// This branch is rejected because the owner-closed macro whole-cover surface is
// represented by the ordinary OwnerRealizationProof / MixedOwnerTilingProof
// lattice before terminal fallback is requested.  A second post-terminal proof
// search would reintroduce an independent fallback authority, even if it never
// materialized bytes.  Terminal fallback goes directly to the declared
// TerminalOutOfDomain carrier; any in-domain macro whole-cover case must have
// been accepted by the
// normal proof lattice.

namespace {

/// One preserved directive's A-token frontier and what its state does.
///
/// The frontier says where the directive sits; the classification says what a
/// payload would observe by crossing it.  Both are needed to place edited
/// material that the alignment cannot place on its own.
struct PreservedDirectiveFrontier {
  uint64_t aFrontier = 0;
  /// Every state effect the preserved directive establishes.  A pragma
  /// contributes one; a preserved `#include` contributes one per pragma its
  /// header owns, because re-entering the header re-runs all of them.
  SmallVector<PragmaClassification, 2> classifications;
  /// Macro definitions a preserved `#define`/`#undef` binds.  A payload that
  /// can observe one preprocesses differently on either side of the directive,
  /// so the binding is what a straddling zone would observe by crossing it.
  SmallVector<MacroStateBinding, 2> macroBindings;
};

/// Return the macro-state observation answer for the names a `push_macro` or
/// `pop_macro` classification binds.
///
/// `push_macro`/`pop_macro` change the definition bound to one macro name --
/// exactly the state a `#define` changes -- so the same proof answers both.
/// The classification records the name but nothing about either definition it
/// swaps between, so the binding carries no producer record and takes the
/// conservative identifier-token observation mode.  Anything but
/// `MacroStateStack` leaves the answer unproven, which the taxonomy ignores.
MacroStateObservationAnswer macroStateStackObservation(
    const RefoldMacroStateProof &macroStateProof,
    const PragmaClassification &classification, StringRef payload) {
  if (classification.effect != PragmaStateEffect::MacroStateStack)
    return MacroStateObservationAnswer::Unproven;

  SmallVector<MacroStateBinding, 2> bindings;
  for (StringRef name : classification.namedIdentifiers)
    bindings.push_back(MacroStateBinding{name, nullptr});
  return macroStateProof.PayloadObservesMacroStateBindings(bindings, payload)
             ? MacroStateObservationAnswer::Observed
             : MacroStateObservationAnswer::Unobserved;
}

/// Return whether `payload` can observe any state the preserved directive
/// establishes.  An unclassified effect observes everything, so a directive
/// with no recorded classification is never insensitive.
bool payloadObservesPreservedDirectiveState(
    const RefoldMacroStateProof &macroStateProof,
    const PreservedDirectiveFrontier &frontier, StringRef payload,
    const LangOptions &lang) {
  if (frontier.classifications.empty() && frontier.macroBindings.empty())
    return true;
  for (const PragmaClassification &classification : frontier.classifications)
    if (payloadObservesPragmaState(
            classification, payload, lang,
            macroStateStackObservation(macroStateProof, classification,
                                       payload)))
      return true;
  if (frontier.macroBindings.empty())
    return false;
  return macroStateProof.PayloadObservesMacroStateBindings(
      frontier.macroBindings, payload);
}

/// Which side of a preserved directive an observing zone may be realized on.
enum class ObservingZonePlacement {
  /// No side is proven, so the partition fails closed.
  Undetermined,
  /// Only the side before the directive is admissible.
  ForcedBefore,
};

/// Decide the side of a zone that *observes* the directive's state.
///
/// Observation is not the same as ambiguity. A zone is placeable whenever the
/// set of sides that are both legal and reproduce B is a singleton, and
/// observation is one of the ways that set shrinks to one rather than a reason
/// to give up. Three situations reach a verdict, and they need different
/// evidence:
///
/// * the zone does not observe at all -- both sides are equivalent, which the
///   caller admits before reaching here;
/// * exactly one side is *legal* -- decided below, for `PoisonIdentifiers`;
/// * both sides are legal but only one reproduces B -- needs macro-liveness
///   facts and is not decided here.
///
/// `PoisonIdentifiers` is the second case. `GCC poison` is consumed by the
/// preprocessor and emits no token, so both placements produce the *same* token
/// stream and the difference is purely legality: naming a poisoned identifier
/// after the directive is an error, naming it before is not. A zone naming one
/// is therefore realizable on exactly one side, and it is the side a determined
/// zone already takes, so admitting it commits to nothing new.
///
/// Everything else stays `Undetermined`, and the restriction is the point.
/// `MacroStateStack` observation is not a legality question -- such a zone is
/// legal on both sides and merely expands differently, so its side is decided
/// by which one reproduces B, which needs facts this classification does not
/// carry. An unclassified directive observes everything and is never admitted.
/// A future kind may well be forced the other way: `pop_macro` restores a
/// definition, so a zone needing the restored one would be forced *after*,
/// which is why this returns a side rather than a yes/no.
/// The side a single observed effect admits.
///
/// Enumerated exhaustively so that adding a pragma kind is a decision here
/// rather than a silent fall-through to the unsafe answer.
ObservingZonePlacement placementForObservedEffect(PragmaStateEffect effect) {
  switch (effect) {
  case PragmaStateEffect::PoisonIdentifiers:
    // Consumed, so both sides emit the same tokens; only the earlier side is
    // legal.
    return ObservingZonePlacement::ForcedBefore;

  case PragmaStateEffect::MacroStateStack:
    // Legal on both sides, expanding differently. Deciding it means proving
    // which side reproduces B, which needs macro-liveness facts.
    return ObservingZonePlacement::Undetermined;

  case PragmaStateEffect::IncludeOnce:
    // Reported observing when the zone could introduce a directive at all.
    // Both sides are legal and which reproduces B depends on what that
    // directive would include, so nothing here fixes a side.
    return ObservingZonePlacement::Undetermined;

  case PragmaStateEffect::Unknown:
    // The unsafe answer by construction; an unrecognized spelling proves
    // nothing about either side.
    return ObservingZonePlacement::Undetermined;

  case PragmaStateEffect::NoState:
  case PragmaStateEffect::DiagnosticState:
  case PragmaStateEffect::SystemHeader:
    // These never report observation, so a zone never reaches here through
    // them. Answer conservatively rather than assert, because the verdict is
    // only consulted after an observation was already reported.
    return ObservingZonePlacement::Undetermined;
  }
  return ObservingZonePlacement::Undetermined;
}

ObservingZonePlacement observingZonePlacement(
    const RefoldMacroStateProof &macroStateProof,
    const PreservedDirectiveFrontier &frontier, StringRef zone,
    const LangOptions &lang) {
  // An unclassified frontier observes everything and proves nothing.
  if (frontier.classifications.empty() && frontier.macroBindings.empty())
    return ObservingZonePlacement::Undetermined;

  // A zone naming a macro the preserved directive binds is legal on both sides
  // and merely preprocesses differently, so its side is decided by which one
  // reproduces B rather than by legality.  A `#define` would force the zone
  // before it -- the name must survive unexpanded to appear in B at all -- and
  // an `#undef` would force it after, for the same reason read the other way.
  // Deciding that needs the definition's liveness at the split, which this
  // frontier does not carry, so it stays undetermined.
  if (!frontier.macroBindings.empty() &&
      macroStateProof.PayloadObservesMacroStateBindings(frontier.macroBindings,
                                                        zone)) {
    return ObservingZonePlacement::Undetermined;
  }

  // A preserved `#include` contributes one effect per pragma its header owns,
  // so several may be observed at once. Every observed effect must admit the
  // same side: one effect forcing a side the next forbids leaves no placement
  // at all, which is the undetermined answer rather than a preference.
  std::optional<ObservingZonePlacement> agreed;
  for (const PragmaClassification &classification : frontier.classifications) {
    if (!payloadObservesPragmaState(
            classification, zone, lang,
            macroStateStackObservation(macroStateProof, classification, zone)))
      continue;

    const ObservingZonePlacement placement =
        placementForObservedEffect(classification.effect);
    if (placement == ObservingZonePlacement::Undetermined)
      return ObservingZonePlacement::Undetermined;
    if (agreed && *agreed != placement)
      return ObservingZonePlacement::Undetermined;
    agreed = placement;
  }

  return agreed.value_or(ObservingZonePlacement::Undetermined);
}

/// Derive where each preserved tokenless directive cuts the realized B payload.
///
/// A closure that preserves a directive in place must emit the B material that
/// belongs before it, then the directive's own source bytes, then the B
/// material that belongs after it.  A directive sits between A tokens
/// `frontier - 1` and `frontier`, so a B token aligned to an A token below the
/// frontier belongs before it and one aligned at or above belongs after.
///
/// Alignment alone does not place every token.  Edited material is unaligned:
/// it has no A-side position, so nothing in the alignment says which side it
/// belongs on.  Such a token is still *determined* when the aligned tokens
/// around it agree -- monotonicity forces material between two before-tokens to
/// be before -- and the only genuinely ambiguous material is what lies strictly
/// between the last determined-before anchor and the first determined-after
/// one.  That zone is where a payload straddles the directive.
///
/// The zone is admitted by insensitivity, not by choice.  When re-preprocessing
/// the zone's own bytes cannot observe the state the directive changes, both
/// placements produce the same translation unit, so committing to one is a
/// proof rather than a guess -- the same move the alignment's semantic resolver
/// makes for an anchor whose alternatives are equivalent.  The commitment is
/// deterministic: the zone goes before the directive.  A sensitive zone, or one
/// whose directive is unclassified, fails closed exactly as before.
///
/// A split is a seam at protected preprocessing state, so the anchors bracketing
/// it must be forced by the core objective rather than merely selected.  A
/// forced anchor is used by every core-optimal alignment, so no other
/// admissible alignment moves it across the directive, and monotonicity then
/// fixes the side of every remaining determined token.  A resolver-authorized
/// anchor cannot stand in: its witness holds that the remaining explanations
/// agree on one normalized edit realization, which is a claim about the
/// contiguous realization it was proved against, not about this partitioned
/// one.
///
/// Splits are returned in `frontiers` order and are non-decreasing.
std::optional<SmallVector<size_t, 4>> deriveBPayloadSplitsAtATokenFrontiers(
    const RefoldMacroStateProof &macroStateProof,
    ArrayRef<int64_t> abTokMapB2A,
    ArrayRef<diffutils::LcsAnchorProof> abTokAnchorProofs,
    ArrayRef<PreservedDirectiveFrontier> frontiers, size_t bMaterialBegin,
    size_t bMaterialEnd,
    llvm::function_ref<StringRef(size_t, size_t)> sliceBSource,
    const LangOptions &lang, std::string *reason) {
  auto refuse =
      [&](std::string detail) -> std::optional<SmallVector<size_t, 4>> {
    if (reason)
      *reason = std::move(detail);
    return std::nullopt;
  };

  if (frontiers.empty() || bMaterialEnd < bMaterialBegin)
    return refuse("no preserved directive frontier to split at");

  // Resolve every B token's aligned A index once; -1 marks edited material with
  // no A-side position.  The material range is usually a handful of tokens, and
  // the same census answers each frontier.
  SmallVector<int64_t, 16> alignedA;
  alignedA.reserve(bMaterialEnd - bMaterialBegin);
  std::optional<int64_t> previousAligned;
  for (size_t bTok = bMaterialBegin; bTok < bMaterialEnd; ++bTok) {
    if (bTok >= abTokMapB2A.size())
      return refuse(
          llvm::formatv("B token {0} is outside the alignment map", bTok)
              .str());
    const int64_t mappedA = abTokMapB2A[bTok];
    if (mappedA >= 0) {
      if (previousAligned && mappedA <= *previousAligned)
        return refuse(llvm::formatv("B token {0} aligns to A token {1}, which "
                                    "is not monotone after A token {2}",
                                    bTok, mappedA, *previousAligned)
                          .str());
      previousAligned = mappedA;
    }
    alignedA.push_back(mappedA);
  }

  // Return whether the alignment anchor at one material B token is forced by
  // the core objective.  `offset` is relative to `bMaterialBegin`.
  auto materialAnchorIsCoreForced = [&](size_t offset) -> bool {
    const int64_t aTok = alignedA[offset];
    if (aTok < 0 || static_cast<size_t>(aTok) >= abTokAnchorProofs.size())
      return false;
    return abTokAnchorProofs[static_cast<size_t>(aTok)].kind ==
           diffutils::LcsAnchorProofKind::CoreOptimalPathForced;
  };

  SmallVector<size_t, 4> splits;
  splits.reserve(frontiers.size());
  size_t previousSplit = bMaterialBegin;
  for (const PreservedDirectiveFrontier &frontier : frontiers) {
    // Bracket the directive with the determined anchors on either side.
    std::optional<size_t> lastBefore;
    size_t firstAfter = bMaterialEnd;
    for (size_t offset = 0; offset < alignedA.size(); ++offset) {
      const int64_t aTok = alignedA[offset];
      if (aTok < 0)
        continue;
      if (static_cast<uint64_t>(aTok) < frontier.aFrontier) {
        lastBefore = offset;
        continue;
      }
      firstAfter = bMaterialBegin + offset;
      break;
    }

    if (lastBefore && bMaterialBegin + *lastBefore >= firstAfter)
      return refuse(llvm::formatv("determined anchors bracketing A frontier "
                                  "{0} are not ordered",
                                  frontier.aFrontier)
                        .str());

    // Everything strictly between the brackets is unaligned by construction:
    // an aligned token there would have moved one of the two brackets.
    const size_t zoneBegin =
        lastBefore ? bMaterialBegin + *lastBefore + 1 : bMaterialBegin;
    const size_t split = firstAfter;

    if (zoneBegin < split) {
      const StringRef zone = sliceBSource(zoneBegin, split);
      if (payloadObservesPreservedDirectiveState(macroStateProof, frontier,
                                                 zone, lang)) {
        // Observing is not the same as ambiguous. An observed effect can leave
        // exactly one admissible side, in which case the zone is placed by
        // proof rather than by preference.
        if (observingZonePlacement(macroStateProof, frontier, zone, lang) !=
            ObservingZonePlacement::ForcedBefore) {
          return refuse(
              llvm::formatv("edited B material [{0},{1}) has no determined side "
                            "of the preserved directive and observes its {2} "
                            "state",
                            zoneBegin, split,
                            frontier.classifications.empty()
                                ? StringRef("unclassified")
                                : toString(frontier.classifications.front()
                                               .effect))
                  .str());
        }

        REFOLD_LOG_TRACE(
            "fallback",
            "edited B material [{0},{1}) observes the preserved directive's "
            "{2} state, which admits only the side before it; placing it there "
            "by proof rather than by preference",
            zoneBegin, split,
            toString(frontier.classifications.front().effect));
      }
    }

    if (split < previousSplit)
      return refuse(llvm::formatv("preserved directive frontiers derive "
                                  "non-monotone B splits {0} after {1}",
                                  split, previousSplit)
                        .str());

    if (lastBefore && !materialAnchorIsCoreForced(*lastBefore))
      return refuse(
          llvm::formatv("B token {0} left of the split at A frontier {1} is "
                        "not a core-forced alignment anchor",
                        bMaterialBegin + *lastBefore, frontier.aFrontier)
              .str());
    if (split < bMaterialEnd &&
        !materialAnchorIsCoreForced(split - bMaterialBegin))
      return refuse(
          llvm::formatv("B token {0} right of the split at A frontier {1} is "
                        "not a core-forced alignment anchor",
                        split, frontier.aFrontier)
              .str());

    previousSplit = split;
    splits.push_back(split);
  }
  return splits;
}

/// Include-tree adjacency facts shared by the include-closure resolvers below.
///
/// `childrenByParent` is built by one walk over GetIncludes(), so each bucket
/// lists an include's children in exactly the relative order the previous full
/// scan visited them.
///
/// `parentGraphIsAcyclic` records whether every include's parent chain
/// terminates.  The resolvers memoize a per-include answer only when it holds:
/// with a cycle present, a walk's answer is cut at whichever node the walk
/// happened to re-enter first, so it is a property of the entry point rather
/// than of the include, and caching it would leak one entry's cut into another
/// entry's proof.  A cyclic graph therefore falls back to the exact
/// uncached traversal.
struct IncludeTreeAdjacency {
  DenseMap<uint64_t, SmallVector<const RefoldModel::IncludeItem *, 4>>
      childrenByParent;
  bool parentGraphIsAcyclic = true;

  /// Return the direct children of \p inc in producer include order.
  ArrayRef<const RefoldModel::IncludeItem *>
  ChildrenOf(const RefoldModel::IncludeItem &inc) const {
    auto it = childrenByParent.find(inc.id);
    if (it == childrenByParent.end())
      return {};
    return it->second;
  }
};

/// Build the include-tree adjacency facts for \p model.
IncludeTreeAdjacency buildIncludeTreeAdjacency(const RefoldModel &model) {
  IncludeTreeAdjacency adjacency;

  for (const RefoldModel::IncludeItem &inc : model.GetIncludes()) {
    if (inc.parent)
      adjacency.childrenByParent[*inc.parent].push_back(&inc);
  }

  // Each include has at most one parent, so the parent relation is a functional
  // graph and a single colored walk per include settles acyclicity in O(I).
  enum class WalkState : uint8_t { Unvisited, OnPath, Settled };
  DenseMap<uint64_t, WalkState> state;
  SmallVector<uint64_t, 16> path;

  for (const RefoldModel::IncludeItem &inc : model.GetIncludes()) {
    path.clear();
    std::optional<uint64_t> cur = inc.id;
    while (cur) {
      WalkState &curState = state[*cur];
      if (curState == WalkState::Settled)
        break;
      if (curState == WalkState::OnPath) {
        adjacency.parentGraphIsAcyclic = false;
        break;
      }
      curState = WalkState::OnPath;
      path.push_back(*cur);

      const RefoldModel::IncludeItem *node = model.GetIncludeById(*cur);
      cur = node ? node->parent : std::nullopt;
    }
    for (uint64_t settled : path)
      state[settled] = WalkState::Settled;
  }

  return adjacency;
}

/// Resolves the transitive side-effect proof for a zero-token include closure.
///
/// The trusted inputs are the immutable refold model, path-identity service, and
/// the caller's deliberately narrow pragma-state predicate.  The resolver owns
/// only recursion-local cycle state: it preserves the original include/directive
/// traversal order and returns the first diagnostic reason encountered.  Cyclic
/// include-parent metadata is treated as an unprovable closure and therefore
/// fails closed rather than authorizing deletion of uncertain source state.
class RecordedIncludeSideEffectResolver {
public:
  RecordedIncludeSideEffectResolver(
      const RefoldModel &model, const RefoldPathIdentity &paths,
      llvm::function_ref<bool(const RefoldModel::PragmaDirective &)>
          pragmaIsConsumableIncludeLocalState)
      : model_(model), paths_(paths),
        pragmaIsConsumableIncludeLocalState_(
            pragmaIsConsumableIncludeLocalState),
        adjacency_(buildIncludeTreeAdjacency(model)) {}

  /// Return the first recorded reason that prevents treating `inc` and its
  /// descendants as a consumable zero-token source gap, or std::nullopt when the
  /// include tree is source-neutral for this closure proof.
  std::optional<std::string>
  FindReason(const RefoldModel::IncludeItem &inc) const {
    DenseSet<uint64_t> visiting;
    return FindReasonImpl(inc, visiting, /*wantReason=*/true);
  }

  /// Return whether any recorded reason prevents the same closure.
  ///
  /// This runs the identical traversal as FindReason but skips diagnostic
  /// spelling, so a boolean query never pays for reason construction, and
  /// caches the per-include answer when the include graph is acyclic.
  bool HasReason(const RefoldModel::IncludeItem &inc) const {
    DenseSet<uint64_t> visiting;
    return FindReasonImpl(inc, visiting, /*wantReason=*/false).has_value();
  }

private:
  /// Build the rejection carrier for one walk.
  ///
  /// A predicate-only walk skips the diagnostic spelling entirely; the empty
  /// string then stands for "rejected, reason not requested".  Both walks run
  /// the same traversal, so the predicate can never disagree with the reason a
  /// diagnostic walk would report.
  static std::optional<std::string>
  Rejected(bool wantReason, llvm::function_ref<std::string()> buildReason) {
    if (!wantReason)
      return std::string();
    return buildReason();
  }

  std::optional<std::string>
  FindReasonImpl(const RefoldModel::IncludeItem &inc,
                 DenseSet<uint64_t> &visiting, bool wantReason) const {
    // A cached answer is only consulted for the predicate walk, and only when
    // the include graph is acyclic; see IncludeTreeAdjacency.  The lookup
    // precedes the cycle guard below, which cannot fire in an acyclic graph.
    const bool memoize = !wantReason && adjacency_.parentGraphIsAcyclic;
    if (memoize) {
      auto cached = rejectedCache_.find(inc.id);
      if (cached != rejectedCache_.end())
        return cached->second ? std::optional<std::string>(std::string())
                              : std::nullopt;
    }

    // A cycle would indicate malformed include-parent metadata.  Do not try to
    // prove closure through it; reject rather than deleting uncertain state.
    if (!visiting.insert(inc.id).second)
      return Rejected(wantReason, [&] {
        return llvm::formatv("include-parent cycle reaches include id={0}",
                             inc.id)
            .str();
      });

    auto finish = [&](std::optional<std::string> reason) {
      visiting.erase(inc.id);
      if (memoize)
        rejectedCache_[inc.id] = reason.has_value();
      return reason;
    };

    // Header declarations are semantic structure, even if this include did not
    // contribute tokens to the particular A-side hunk being refolded.
    if (!inc.decls.empty())
      return finish(Rejected(wantReason, [&] {
        return llvm::formatv("include id={0} path='{1}' has {2} "
                             "recorded header declaration(s)",
                             inc.id, IncludePathForDiagnostic(inc),
                             inc.decls.size())
            .str();
      }));

    // A nested include is not inherently a side effect.  It is consumable when
    // its own expansion is empty and its descendants/directives are consumable
    // by the same zero-token include-gap proof.  Record which child blocked the
    // proof so the mixed-closure rejection points at the real source structure.
    for (const RefoldModel::IncludeItem *child : adjacency_.ChildrenOf(inc)) {
      if (child->cover.IsValid())
        return finish(Rejected(wantReason, [&] {
          return llvm::formatv("child include id={0} path='{1}' "
                               "materializes PP cover=[{2},{3})",
                               child->id, IncludePathForDiagnostic(*child),
                               child->cover.begin, child->cover.end)
              .str();
        }));
      if (std::optional<std::string> childReason =
              FindReasonImpl(*child, visiting, wantReason))
        return finish(Rejected(wantReason, [&] {
          return llvm::formatv("child include id={0} path='{1}' rejected: "
                               "{2}",
                               child->id, IncludePathForDiagnostic(*child),
                               *childReason)
              .str();
        }));
    }

    // Include-owned #define/#undef directives are macro-state transitions, not
    // PP tokens.  When their containing zero-token include lies inside the
    // replacement envelope, the directive is consumed with that envelope.  Any
    // surviving source that still observes the removed macro state is handled
    // later by the macro-state liveness repair passes: consumed #defines can be
    // preserved or replaced with whole-cover callsite realizations, and
    // consumed #undefs can be preserved when removing them would resurrect an
    // earlier definition.  Directives outside that nameable macro-state class
    // remain side-effect-bearing structure that this closure must not erase.
    for (const auto &directive : model_.GetMacroDirectives()) {
      if (directive.ownerIncludeId && *directive.ownerIncludeId == inc.id &&
          !directive.IsMacroStateDirective()) {
        return finish(Rejected(wantReason, [&] {
          return llvm::formatv("include id={0} path='{1}' owns "
                               "non-consumable macro directive id={2} "
                               "subkind='{3}' text='{4}'",
                               inc.id, IncludePathForDiagnostic(inc),
                               directive.id, directive.subkind,
                               stringutils::showWsWithClip(directive.text, 120))
              .str();
        }));
      }
    }

    // Conditional control directives are also source structure, but an
    // include-local conditional group with no materialized A tokens is source
    // order only: it contributed no PP material and any real side effects
    // inside the group are represented separately as macro directives, child
    // includes, pragmas, or declarations.  When such a group lies inside the
    // source envelope being replaced, consuming it is the same kind of closure
    // as consuming an empty include gap.  Reject only a conditional group that
    // owns materialized tokens; that would make this a token-producing include,
    // not a zero-token gap.
    for (const auto &group : model_.GetConds()) {
      if (!group.parentIncludeId || *group.parentIncludeId != inc.id)
        continue;

      for (const RefoldModel::CondArm &arm : group.arms) {
        if (arm.span && arm.span->IsValid() &&
            arm.span->begin < arm.span->end) {
          return finish(Rejected(wantReason, [&] {
            return llvm::formatv("include id={0} path='{1}' owns "
                                 "conditional group id={2} arm id={3} "
                                 "with materialized PP span=[{4},{5})",
                                 inc.id, IncludePathForDiagnostic(inc),
                                 group.id, arm.id, arm.span->begin,
                                 arm.span->end)
                .str();
          }));
        }
      }
    }

    // Pragmas are keyed by physical file path rather than include id, so check
    // the resolved header path when available.  Unknown pragmas are always
    // side-effect-bearing here: they can affect diagnostics, layout,
    // optimization, visibility, attributes, or later preprocessing state while
    // contributing no PP tokens.  The only pragma admitted by this closure is
    // an explicitly parsed `#pragma once` in an otherwise source-neutral
    // zero-token include.  Anything richer must wait for a pragma-state proof
    // or a repair mechanism analogous to macro-state liveness.
    if (inc.resolvedPath)
      for (const auto &pragma : model_.GetPragmas()) {
        if (!paths_.PathsEqual(pragma.sitePath, *inc.resolvedPath))
          continue;
        if (!pragmaIsConsumableIncludeLocalState_(pragma)) {
          return finish(Rejected(wantReason, [&] {
            return llvm::formatv("include id={0} path='{1}' contains "
                                 "non-consumable pragma id={2} text='{3}'",
                                 inc.id, IncludePathForDiagnostic(inc),
                                 pragma.id,
                                 stringutils::showWsWithClip(pragma.text, 120))
                .str();
          }));
        }
        if (IncludeHasNonPragmaStructure(inc)) {
          return finish(Rejected(wantReason, [&] {
            return llvm::formatv("include id={0} path='{1}' contains "
                                 "#pragma once with additional recorded "
                                 "source structure",
                                 inc.id, IncludePathForDiagnostic(inc))
                .str();
          }));
        }
      }

    return finish(std::nullopt);
  }

  StringRef IncludePathForDiagnostic(
      const RefoldModel::IncludeItem &inc) const {
    return inc.resolvedPath ? *inc.resolvedPath : inc.target;
  }

  /// Return true iff the include has structure other than an explicitly
  /// consumable pragma.  This keeps the pragma-once exception fail-closed when
  /// the include also owns macro state, conditional structure, children, or
  /// declarations, while preserving the model traversal order used by the old
  /// local proof lambda.
  bool IncludeHasNonPragmaStructure(
      const RefoldModel::IncludeItem &inc) const {
    if (!inc.decls.empty())
      return true;

    if (!adjacency_.ChildrenOf(inc).empty())
      return true;

    for (const auto &directive : model_.GetMacroDirectives())
      if (directive.ownerIncludeId && *directive.ownerIncludeId == inc.id)
        return true;

    for (const auto &group : model_.GetConds())
      if (group.parentIncludeId && *group.parentIncludeId == inc.id)
        return true;

    return false;
  }

  const RefoldModel &model_;
  const RefoldPathIdentity &paths_;
  llvm::function_ref<bool(const RefoldModel::PragmaDirective &)>
      pragmaIsConsumableIncludeLocalState_;
  IncludeTreeAdjacency adjacency_;

  /// Predicate-walk results keyed by include id.  Only populated when the
  /// include parent graph is acyclic; see IncludeTreeAdjacency.
  mutable DenseMap<uint64_t, bool> rejectedCache_;
};

/// Resolves whether a complete include directive may be preserved inside a
/// conditional source island while its subtree carries modeled macro-state.
///
/// Trusted inputs are the immutable refold model, path identity, macro-state
/// proof service, source mapper, current token hunk, and the same narrow pragma
/// predicate used by the zero-token include closure proof.  The resolver owns
/// only recursion-local cycle state.  It preserves child/directive/conditional/
/// macro/pragma ordering and fails closed for declarations, token-producing
/// descendants, unsupported pragmas, non-macro-state structure, and replacement
/// text that would observe macro state introduced only after the payload.
class ConditionalStateIncludePreservationResolver {
public:
  ConditionalStateIncludePreservationResolver(
      const RefoldModel &model, const RefoldPathIdentity &paths,
      const RefoldMacroStateProof &macroStateProof,
      const RefoldSourceMapper &sourceMapper, const diffutils::Hunk &h,
      llvm::function_ref<bool(const RefoldModel::PragmaDirective &)>
          pragmaIsConsumableIncludeLocalState)
      : model_(model), paths_(paths), macroStateProof_(macroStateProof),
        sourceMapper_(sourceMapper), h_(h),
        pragmaIsConsumableIncludeLocalState_(
            pragmaIsConsumableIncludeLocalState),
        adjacency_(buildIncludeTreeAdjacency(model)) {}

  /// Return true iff the include's directive can remain in the preserved
  /// conditional island without requiring a new raw-B fallback authority.
  bool IsPreservable(const RefoldModel::IncludeItem &inc) const {
    DenseSet<uint64_t> visiting;
    return IsPreservableImpl(inc, visiting);
  }

private:
  bool IsPreservableImpl(const RefoldModel::IncludeItem &inc,
                         DenseSet<uint64_t> &visiting) const {
    if (inc.cover.IsValid())
      return false;

    // A cached answer is only consulted when the include graph is acyclic; see
    // IncludeTreeAdjacency.  The lookup precedes the cycle guard below, which
    // cannot fire in an acyclic graph.
    const bool memoize = adjacency_.parentGraphIsAcyclic;
    if (memoize) {
      auto cached = preservableCache_.find(inc.id);
      if (cached != preservableCache_.end())
        return cached->second;
    }

    if (!visiting.insert(inc.id).second)
      return false;

    auto finish = [&](bool result) {
      visiting.erase(inc.id);
      if (memoize)
        preservableCache_[inc.id] = result;
      return result;
    };

    if (!inc.decls.empty())
      return finish(false);

    for (const RefoldModel::IncludeItem *child : adjacency_.ChildrenOf(inc)) {
      if (!IsPreservableImpl(*child, visiting))
        return finish(false);
    }

    for (const auto &directive : model_.GetMacroDirectives()) {
      if (!directive.ownerIncludeId)
        continue;
      if (!IncludeOwnsOrContains(inc, *directive.ownerIncludeId))
        continue;

      if (HunkReplacementObservesMacroStateDirective(directive))
        return finish(false);
    }

    for (const auto &group : model_.GetConds()) {
      if (!group.parentIncludeId)
        continue;
      if (!IncludeOwnsOrContains(inc, *group.parentIncludeId))
        continue;
      for (const RefoldModel::CondArm &arm : group.arms)
        if (arm.span && arm.span->IsValid() && arm.span->begin < arm.span->end)
          return finish(false);
    }

    for (const auto &macro : model_.GetMacroInvocations()) {
      if (!macro.ownerIncludeId)
        continue;
      if (!IncludeOwnsOrContains(inc, *macro.ownerIncludeId))
        continue;
      if (macro.cover.IsValid())
        return finish(false);
      for (const auto &span : macro.stringifySpans)
        if (span.IsValid())
          return finish(false);
      for (const auto &span : macro.pasteSpans)
        if (span.IsValid())
          return finish(false);
    }

    if (inc.resolvedPath)
      for (const auto &pragma : model_.GetPragmas())
        if (paths_.PathsEqual(pragma.sitePath, *inc.resolvedPath) &&
            !pragmaIsConsumableIncludeLocalState_(pragma))
          return finish(false);

    return finish(true);
  }

  bool HunkReplacementObservesMacroStateDirective(
      const RefoldModel::MacroDirective &directive) const {
    return macroStateProof_.ReplacementObservesMacroStateDirective(
        directive, sourceMapper_.SliceBSource(h_.bStart, h_.bEnd),
        /*unprovenObserves=*/true);
  }

  /// Return true iff \p ownerIncludeId names \p inc itself or a recorded
  /// descendant of it.
  ///
  /// This is the exact ownership domain of the subtree proof: producer facts
  /// attached to a nested include are attributed to the outer include whose
  /// preservation is being decided.  An owner id the model does not name is not
  /// in the domain, matching the previous scan that simply found no match.
  bool IncludeOwnsOrContains(const RefoldModel::IncludeItem &inc,
                             uint64_t ownerIncludeId) const {
    if (ownerIncludeId == inc.id)
      return true;
    const RefoldModel::IncludeItem *owner =
        model_.GetIncludeById(ownerIncludeId);
    return owner && IncludeIsDescendantOf(*owner, inc);
  }

  bool IncludeIsDescendantOf(const RefoldModel::IncludeItem &candidate,
                             const RefoldModel::IncludeItem &root) const {
    std::optional<uint64_t> cur = candidate.parent;
    while (cur) {
      if (*cur == root.id)
        return true;
      const RefoldModel::IncludeItem *parent = model_.GetIncludeById(*cur);
      if (!parent)
        return false;
      cur = parent->parent;
    }
    return false;
  }

  const RefoldModel &model_;
  const RefoldPathIdentity &paths_;
  const RefoldMacroStateProof &macroStateProof_;
  const RefoldSourceMapper &sourceMapper_;
  const diffutils::Hunk &h_;
  llvm::function_ref<bool(const RefoldModel::PragmaDirective &)>
      pragmaIsConsumableIncludeLocalState_;
  IncludeTreeAdjacency adjacency_;

  /// Per-include preservation answers.  Only populated when the include parent
  /// graph is acyclic; see IncludeTreeAdjacency.
  mutable DenseMap<uint64_t, bool> preservableCache_;
};

} // namespace

std::optional<RefoldExpansionFallbackPlanner::TextEdit>
RefoldExpansionFallbackPlanner::BuildTUIncludeClosureEditForUnresolvedHunk(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    ArrayRef<std::pair<uint64_t, uint64_t>> stagedSourceIntervals) const {
  const ExpansionFallbackBranchClassification fallbackBranch =
      ClassifyExpansionFallbackBranch(
          ExpansionFallbackBranchKind::TUIncludeClosureEdit);
  theoremAuditService_.AuditExpansionFallbackBranchClassification(
      fallbackBranch, "BuildTUIncludeClosureEditForUnresolvedHunk");

  // Expansion fallback and structural hunk tiling must reason over the same
  // exact physical preprocessing census.  A mismatched source surface would
  // let the two paths classify identical bytes differently, so this closure
  // proof is unavailable unless the engine-owned TU index names this exact
  // source owner and byte extent.
  if (!paths_.PathsEqual(preprocessingStructureIndex_.GetSourcePath(),
                         tuPath) ||
      preprocessingStructureIndex_.GetOwnerIncludeId() ||
      preprocessingStructureIndex_.GetSourceSize() != tuBytes.size()) {
    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure rejected: shared preprocessing index does not "
        "match TU path='{0}' bytes={1}",
        tuPath, tuBytes.size());
    return std::nullopt;
  }

  // Exact lexical trivia and literal conditional-control preservation are
  // submitted through the shared source-gap theorem.  The old text predicates
  // remain narrow semantic policies, but they no longer maintain a second
  // preprocessing inventory or byte-cover implementation beside structural
  // hunk tiling.
  auto gapIsIndexedLexerTrivia = [&](uint64_t begin, uint64_t end) {
    return proveSourceGapWithIndexedTrivia(
               preprocessingStructureIndex_, begin, end,
               ArrayRef<SourceGapProofPiece>())
        .has_value();
  };
  auto gapIsIndexedPreservableIncludeClosureTrivia =
      [&](uint64_t begin, uint64_t end) {
        return proveSourceGapWithPolicy(
                   preprocessingStructureIndex_, begin, end,
                   ArrayRef<SourceGapProofPiece>(),
                   [&](uint64_t neutralBegin, uint64_t neutralEnd) {
                     return isPreservableIncludeClosureGapTrivia(
                         tuBytes.slice(neutralBegin, neutralEnd));
                   },
                   [](size_t) {}, sourceGapConditionalDirectiveKindMask())
            .has_value();
      };

  // This source-closure path is the declared TUIncludeClosureEdit proof class:
  // one closed TU source interval may replace top-level include directives plus
  // any adjacent TU-owned tokens consumed by the same hunk, provided all
  // state/layout, B-envelope, and composition obligations below are discharged.
  // Stay fail-closed unless the closure is exact or can be widened without
  // absorbing any independent token diff.
  if (h.isInsertOnly()) {
    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure rejected: insert-only hunk A=[{0},{1}) "
        "B=[{2},{3}) is outside the first closure-edit domain",
        h.aStart, h.aEnd, h.bStart, h.bEnd);
    return std::nullopt;
  }

  SmallVector<const RefoldModel::IncludeItem *, 8> touched;
  for (const auto &inc : model_.GetIncludes()) {
    // Only top-level includes spelled directly in the TU are eligible for this
    // first source-closure class. Nested includes already have a dedicated
    // include-materialization path, while non-TU sites would need a different
    // embedding proof.
    if (inc.parent || !paths_.PathsEqual(inc.sitePath, tuPath))
      continue;
    if (inc.cover.end <= h.aStart || inc.cover.begin >= h.aEnd)
      continue;
    touched.push_back(&inc);
  }

  if (touched.empty()) {
    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure rejected: no top-level TU include touches hunk "
        "A=[{0},{1})",
        h.aStart, h.aEnd);
    return std::nullopt;
  }

  // Process touched includes in TU order so the closure can be proven as one
  // contiguous include cover rather than as a set of unrelated include ranges.
  llvm::sort(touched, [](const RefoldModel::IncludeItem *lhs,
                         const RefoldModel::IncludeItem *rhs) {
    if (lhs->siteB != rhs->siteB)
      return lhs->siteB < rhs->siteB;
    return lhs->id < rhs->id;
  });

  const RefoldModel::IncludeItem *first = touched.front();
  const RefoldModel::IncludeItem *last = touched.back();

  uint64_t coverBegin = std::numeric_limits<uint64_t>::max();
  uint64_t coverEnd = 0;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->cover.end <= inc->cover.begin) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure rejected: touched include inc#{0} has empty "
          "or invalid A-cover [{1},{2})",
          inc->id, inc->cover.begin, inc->cover.end);
      return std::nullopt;
    }
    coverBegin = std::min(coverBegin, inc->cover.begin);
    coverEnd = std::max(coverEnd, inc->cover.end);
  }

  // Pure include closure requires touched include covers to form one contiguous
  // A-token run so their B material can be projected as a single include
  // realization.  Mixed TU/include closure is broader.  It may touch several
  // include runs separated by TU-owned material, but only if the later whole-
  // envelope proof consumes that intervening material as part of the same
  // source replacement.  Do not reject split include runs here; treating them
  // as mixed gives the material-gap verifier a chance to prove or reject
  // the complete source interval deterministically.
  SmallVector<const RefoldModel::IncludeItem *, 8> coverOrdered = touched;
  llvm::sort(coverOrdered, [](const RefoldModel::IncludeItem *lhs,
                              const RefoldModel::IncludeItem *rhs) {
    if (lhs->cover.begin != rhs->cover.begin)
      return lhs->cover.begin < rhs->cover.begin;
    if (lhs->cover.end != rhs->cover.end)
      return lhs->cover.end < rhs->cover.end;
    return lhs->id < rhs->id;
  });

  bool includeCoversContiguous = true;
  uint64_t expectedCoverBegin = coverBegin;
  for (const RefoldModel::IncludeItem *inc : coverOrdered) {
    if (inc->cover.begin != expectedCoverBegin) {
      includeCoversContiguous = false;
      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure treating touched includes as mixed: split "
          "A-cover at inc#{0} cover=[{1},{2}) expectedBegin={3}",
          inc->id, inc->cover.begin, inc->cover.end, expectedCoverBegin);
      break;
    }
    expectedCoverBegin = inc->cover.end;
  }

  // `exactCover` means the edit already matches the include closure. The
  // wider `hunkInsideCover` case is a controlled widening: the hunk may be
  // absorbed only if later checks prove the wider include closure is safe.
  // Split include runs are always mixed, even when the hunk lies inside the
  // min/max include cover, because the intervening TU-owned material must be
  // consumed by the whole-envelope proof rather than by pure include
  // projection.
  const bool exactCover =
      includeCoversContiguous && coverBegin == h.aStart && coverEnd == h.aEnd;
  const bool hunkInsideCover =
      includeCoversContiguous && coverBegin <= h.aStart && h.aEnd <= coverEnd;

  // Return true when `cand` would be absorbed by the closed A-side range
  // [begin, end]. Token-consuming edits use interval overlap; pure insertions
  // use the closed insertion-gap test.
  auto hunkTouchesAClosedRange = [](const diffutils::Hunk &cand, uint64_t begin,
                                    uint64_t end) -> bool {
    if (end < begin)
      end = begin;

    // A-consuming edits touch the extra cover when their token intervals
    // overlap it. Pure insertions touch the cover when their insertion anchor
    // lies in the closed gap interval: a widened include materialization would
    // otherwise absorb the insertion payload without going through the normal
    // candidate-conflict law.
    if (cand.aStart < cand.aEnd)
      return cand.aStart < end && begin < cand.aEnd;
    return cand.bStart < cand.bEnd && cand.aStart >= begin &&
           cand.aStart <= end;
  };

  // Return true when the proposed widened A range would cover any other token
  // diff hunk besides the hunk currently being realized.
  auto rangeHasForeignTokenDiff = [&](uint64_t begin, uint64_t end) -> bool {
    for (const diffutils::Hunk &cand : abTokHunks_) {
      if (cand == h)
        continue;
      if (hunkTouchesAClosedRange(cand, begin, end))
        return true;
    }
    return false;
  };

  // Return true when the proposed widened source range would overlap an edit
  // that has already been staged. Zero-width staged edits are treated as closed
  // insertion points.
  auto sourceTouchesStagedEdit = [&](uint64_t begin, uint64_t end) -> bool {
    for (const auto &interval : stagedSourceIntervals) {
      if (interval.first < interval.second) {
        if (interval.first < end && begin < interval.second)
          return true;
        continue;
      }

      if (begin <= interval.first && interval.first <= end)
        return true;
    }
    return false;
  };

  // The include-only closure case below materializes exactly the include cover.
  // A mixed TU/include hunk is wider: the edit consumes tokens on both sides of
  // the include cover, so the A materialization range must close over the whole
  // touched token interval.
  const uint64_t closureBegin = std::min(h.aStart, coverBegin);
  const uint64_t closureEnd = std::max(h.aEnd, coverEnd);
  const bool mixedTUIncludeClosure = !hunkInsideCover;

  // Defaults for the pure include-closure case.  The mixed case widens both the
  // materialized A token range and the source byte interval below.
  uint64_t materialBeginA = coverBegin;
  uint64_t materialEndA = coverEnd;
  uint64_t sourceBegin = first->siteB;
  uint64_t sourceEnd = last->siteE;

  // Return true iff the PP token is part of one of the include expansions that
  // this closure is explicitly consuming.
  auto tokenCoveredByTouchedInclude = [&](uint64_t pp) -> bool {
    for (const RefoldModel::IncludeItem *inc : touched)
      if (inc->cover.begin <= pp && pp < inc->cover.end)
        return true;
    return false;
  };

  // Return true iff this include directive belongs to the touched include run
  // whose expansion participates in the closure proof.
  auto includeIsTouched = [&](const RefoldModel::IncludeItem &inc) -> bool {
    for (const RefoldModel::IncludeItem *touchedInc : touched)
      if (touchedInc->id == inc.id)
        return true;
    return false;
  };

  // Return true iff the macro invocation is part of the source spelling of a
  // touched include directive.  Macro-expanded include targets, for example
  //
  //   #define HDR "two.inc"
  //   #include HDR
  //
  // are control spelling for the include directive, not independent TU
  // material. When the closure consumes the touched include site, complete
  // macro calls wholly inside that site are consumed with it.  Partial overlaps
  // remain rejected by the ordinary macro-overlap guard below.
  auto macroInvocationIsInsideTouchedIncludeDirective =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    if (!m.invFile || m.invFile->empty() ||
        !paths_.PathsEqual(*m.invFile, tuPath))
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE)
      return false;

    for (const RefoldModel::IncludeItem *inc : touched) {
      if (!paths_.PathsEqual(inc->sitePath, tuPath))
        continue;
      if (inc->siteB <= *m.invB && *m.invE <= inc->siteE)
        return true;
    }
    return false;
  };

  // Return true iff the pragma is explicitly classified as source-neutral when
  // its containing include directive is consumed by a zero-token gap closure.
  //
  // Keep this whitelist deliberately small.  Most pragmas model compiler state
  // rather than token material, and the refold map does not currently carry the
  // state-region information needed to replay or repair them.  `#pragma once`
  // is the one admitted spelling because, for an otherwise source-neutral
  // zero-token include, suppressing future textual inclusion of the same
  // zero-token file cannot change the PP token stream.
  // Return whether a pragma establishes include-once state for its file.
  //
  // This is the narrow question the `#pragma once` reactivation repair asks:
  // consuming the include that entered a once-header can make a later skipped
  // include of the same header live again.  Only `IncludeOnce` answers it, and
  // widening this predicate would make some other header claim once-state and
  // corrupt that repair.
  auto pragmaEstablishesIncludeOnceState =
      [&](const RefoldModel::PragmaDirective &pragma) -> bool {
    return classifyPragmaDirective(pragma.text.trim()).effect ==
           PragmaStateEffect::IncludeOnce;
  };

  // Return whether a pragma's state dies with the include that carries it.
  //
  // A zero-token include inside a replacement envelope is deleted whole, so a
  // pragma it owns is admissible exactly when nothing outside the include can
  // observe the state it changed.  Two classified effects qualify, for
  // different reasons:
  //
  //   * `IncludeOnce` suppresses future textual inclusion of a file that
  //     produced no tokens, which cannot change the PP token stream;
  //   * `SystemHeader` suppresses diagnostics from that point in the containing
  //     file, and the pragma and every byte in its scope are deleted together.
  //
  // Every other effect either outlives the include (`PoisonIdentifiers`,
  // `MacroStateStack`) or is unclassified, and stays side-effect bearing.
  auto pragmaIsConsumableIncludeLocalState =
      [&](const RefoldModel::PragmaDirective &pragma) -> bool {
    switch (classifyPragmaDirective(pragma.text.trim()).effect) {
    case PragmaStateEffect::IncludeOnce:
    case PragmaStateEffect::SystemHeader:
      return true;
    case PragmaStateEffect::Unknown:
    case PragmaStateEffect::NoState:
    case PragmaStateEffect::PoisonIdentifiers:
    case PragmaStateEffect::MacroStateStack:
    case PragmaStateEffect::DiagnosticState:
      return false;
    }
    return false;
  };

  // Does an include's subtree own a pragma whose effect outlives the include?
  //
  // This is the narrower question the *touched* includes need.  Consuming a
  // touched include is justified by the closure realizing its expansion, which
  // accounts for its tokens and, through macro-state liveness repair, for its
  // macro directives.  It does not account for pragma state that survives the
  // include's end.  Deleting an include deletes everything it entered, so the
  // question covers the whole subtree: a pragma two headers down dies exactly
  // as one in the directly included file does.
  auto includeSubtreeOwnsOutlivingPragma =
      [&](const RefoldModel::IncludeItem &root) -> bool {
    llvm::SmallVector<uint64_t, 16> subtree{root.id};
    llvm::DenseSet<uint64_t> visited{root.id};
    for (size_t index = 0; index < subtree.size(); ++index) {
      for (const RefoldModel::IncludeItem &child : model_.GetIncludes()) {
        if (child.parent && *child.parent == subtree[index] &&
            visited.insert(child.id).second) {
          subtree.push_back(child.id);
        }
      }
    }

    for (const RefoldModel::PragmaDirective &pragma : model_.GetPragmas()) {
      if (pragmaIsConsumableIncludeLocalState(pragma))
        continue;
      if (pragma.ownerIncludeId) {
        if (visited.count(*pragma.ownerIncludeId))
          return true;
        continue;
      }
      // Falling back to physical path is deliberately conservative: a header
      // reached both inside and outside the subtree answers yes, which only
      // declines a consumption.
      for (uint64_t includeId : subtree) {
        const RefoldModel::IncludeItem *include =
            model_.GetIncludeById(includeId);
        if (!include)
          continue;
        StringRef openedPath;
        if (include->openedPath && !include->openedPath->empty())
          openedPath = *include->openedPath;
        else if (include->resolvedPath)
          openedPath = *include->resolvedPath;
        if (!openedPath.empty() &&
            paths_.PathsEqual(openedPath, pragma.sitePath)) {
          return true;
        }
      }
    }
    return false;
  };

  const RecordedIncludeSideEffectResolver recordedIncludeSideEffectResolver(
      model_, paths_, pragmaIsConsumableIncludeLocalState);

  auto includeRecordedSideEffectReason =
      [&](const RefoldModel::IncludeItem &inc) -> std::optional<std::string> {
    return recordedIncludeSideEffectResolver.FindReason(inc);
  };

  auto includeHasRecordedSideEffects =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    return recordedIncludeSideEffectResolver.HasReason(inc);
  };

  const ConditionalStateIncludePreservationResolver
      conditionalStateIncludePreservationResolver(
          model_, paths_, macroStateProof_, sourceMapper_, h,
          pragmaIsConsumableIncludeLocalState);

  auto includeIsPreservableConditionalStateInclude =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    return conditionalStateIncludePreservationResolver.IsPreservable(inc);
  };

  // Recursively prove that a zero-token macro invocation is source-neutral.
  // The shared adapter owns the replacement-list recursion and TU-specific
  // policy wiring; this fallback path only supplies the current TU surface.
  const TUSourceNeutralityContext tuSourceNeutrality =
      RefoldSourceNeutralityProof::BuildTUSourceNeutralityContext(
          model_, macroStateProof_, paths_, tuBytes, tuPath,
          isWsOrCompleteCommentTrivia);

  auto macroInvocationIsSourceNeutralZeroToken =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    return RefoldSourceNeutralityProof::MacroInvocationIsSourceNeutralZeroToken(
        tuSourceNeutrality, m);
  };

  // Return true iff the macro invocation is a complete zero-token TU callsite
  // inside the candidate replacement envelope.  Partial overlaps remain
  // non-refoldable here because deleting only part of a macro call would not be
  // a proved source closure.
  auto macroInvocationIsConsumableZeroTokenGap =
      [&](const RefoldModel::MacroInvocation &m, uint64_t begin,
          uint64_t end) -> bool {
    if (m.invFile && !m.invFile->empty() &&
        !paths_.PathsEqual(*m.invFile, tuPath))
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE)
      return false;
    if (*m.invB < begin || end < *m.invE)
      return false;
    return macroInvocationIsSourceNeutralZeroToken(m);
  };

  using MacroDirectiveSourceInterval = MacroStateDirectiveLineInterval;

  // Recover the full physical source interval for a TU-spelled macro-state
  // directive. The shared helper owns the macro-name-anchor reconstruction and
  // exact-byte validation against the TU source bytes.
  auto macroDirectiveFullSourceInterval =
      [&](const RefoldModel::MacroDirective &directive) {
        return macroStateProof_.RecoverMacroStateDirectiveLineInterval(
            directive, tuPath, tuBytes, std::nullopt);
      };

  auto macroDirectiveIsConsumableStateGap =
      [&](const RefoldModel::MacroDirective &directive, uint64_t begin,
          uint64_t end) -> std::optional<MacroDirectiveSourceInterval> {
    std::optional<MacroDirectiveSourceInterval> interval =
        macroDirectiveFullSourceInterval(directive);
    if (!interval)
      return std::nullopt;
    if (interval->begin < begin || end < interval->end)
      return std::nullopt;
    return interval;
  };

  // Return a TU-spelled pragma that is wholly contained in the physical source
  // line being scanned, if the refold map recorded one there.
  //
  // Unlike #define/#undef, pragmas are deliberately not treated as consumable
  // source-neutral artifacts here.  A pragma may mutate compiler state without
  // producing PP tokens, and the refold map currently records only its spelling
  // and source anchors, not the state region needed to prove deletion safe.
  // This helper is therefore diagnostic-only: it lets the rejection log name
  // the exact pragma that blocked the closure instead of reporting an opaque
  // non-trivia gap.
  auto findTUPragmaOnSourceLine =
      [&](uint64_t lineBegin,
          uint64_t lineEnd) -> const RefoldModel::PragmaDirective * {
    for (const auto &pragma : model_.GetPragmas()) {
      if (!paths_.PathsEqual(pragma.sitePath, tuPath))
        continue;
      if (lineBegin <= pragma.siteB && pragma.siteE <= lineEnd &&
          pragma.siteB < pragma.siteE)
        return &pragma;
    }
    return nullptr;
  };

  // Return a concrete diagnostic when a TU gap contains a recorded pragma that
  // blocks closure.  This is checked at the outer rejection site as well as in
  // the line scanner, because a pragma-only gap has no consumable recorded
  // pieces; without this fast path the scanner is never entered and the user
  // only sees a generic "non-trivia gap" rejection.
  auto nonConsumableTUPragmaGapReason =
      [&](uint64_t gapBegin, uint64_t gapEnd) -> std::optional<std::string> {
    for (const auto &pragma : model_.GetPragmas()) {
      if (!paths_.PathsEqual(pragma.sitePath, tuPath))
        continue;
      if (gapBegin <= pragma.siteB && pragma.siteE <= gapEnd &&
          pragma.siteB < pragma.siteE) {
        return llvm::formatv("source gap [{0},{1}) contains non-consumable "
                             "TU pragma id={2} site=[{3},{4}) text='{5}'",
                             gapBegin, gapEnd, pragma.id, pragma.siteB,
                             pragma.siteE,
                             stringutils::showWsWithClip(pragma.text, 120))
            .str();
      }
    }
    return std::nullopt;
  };

  // Return true iff a recorded TU conditional group may be preserved as
  // inert inter-include source in a pure include-closure replacement.
  //
  // This is the preservation-side counterpart to the mixed-envelope conditional
  // consumption proof below.  The static textual parser intentionally accepts
  // only a tiny literal grammar (`#if 0`, `#if 1`, ...), because it has no
  // preprocessor state.  For source forms such as `#ifdef ENABLE_GAP`, the
  // refold map already gives us the deterministic proof object: a complete
  // conditional-group byte interval and the PP spans materialized by each arm.
  //
  // Pure include closure does not delete these bytes.  It projects the touched
  // include material into B and carries source-neutral inter-include control
  // structure forward verbatim.  Therefore a recorded conditional group is
  // preservable when it is complete, TU-spelled, wholly inside the gap, and no
  // arm materialized PP tokens in A.  Active source-bearing artifacts inside
  // the group remain outside this proof; they require their own
  // preservation/repair model rather than being hidden inside an opaque
  // conditional gap.
  auto conditionalGroupIsPreservableIncludeClosureGap =
      [&](const RefoldModel::CondGroup &group, uint64_t gapBegin,
          uint64_t gapEnd) -> bool {
    auto includeIsNeutral = [&](const RefoldModel::IncludeItem &inc) {
      return (!inc.cover.IsValid() && !includeHasRecordedSideEffects(inc)) ||
             includeIsPreservableConditionalStateInclude(inc);
    };
    NeutralConditionalIslandContext islandContext{
        /*requireGroupBeginAtLineStart=*/true,
        NeutralConditionalArmSpanMode::AllArms};
    return RefoldSourceNeutralityProof::ConditionalGroupIsNeutralIsland(
        tuSourceNeutrality, group, gapBegin, gapEnd, islandContext,
        includeIsNeutral);
  };

  // Return true iff a pure include-closure inter-include gap can be preserved
  // verbatim by tiling it with lexical trivia and complete recorded zero-token
  // conditional groups.
  //
  // This extends `isPreservableIncludeClosureGapTrivia()` without weakening its
  // no-heuristics contract.  The directive spelling inside each conditional
  // group can be macro-dependent (`#ifdef`, `#ifndef`, `#if defined(...)`, ...)
  // because the producer has already recorded the complete group boundaries and
  // which arms materialized A tokens for this preprocessing run.  We preserve
  // the original bytes rather than consuming them, so this helper is used only
  // by the pure include-closure gap path.
  auto gapIsPreservableRecordedConditionalIncludeClosure =
      [&](uint64_t gapBegin, uint64_t gapEnd) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    struct ConditionalPiece {
      uint64_t begin;
      uint64_t end;
      uint64_t id;
    };

    SmallVector<ConditionalPiece, 8> pieces;
    for (const auto &group : model_.GetConds()) {
      if (!conditionalGroupIsPreservableIncludeClosureGap(
              group, gapBegin, gapEnd)) {
        continue;
      }
      std::optional<std::pair<uint64_t, uint64_t>> exactRange =
          findSourceGapConditionalGroupRange(preprocessingStructureIndex_,
                                             group.id);
      if (!exactRange || exactRange->first < gapBegin ||
          exactRange->second > gapEnd) {
        continue;
      }
      pieces.push_back({exactRange->first, exactRange->second, group.id});
    }

    if (pieces.empty())
      return false;

    SmallVector<SourceGapProofPiece, 8> proofPieces;
    proofPieces.reserve(pieces.size());
    for (size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
      const ConditionalPiece &piece = pieces[pieceIndex];
      proofPieces.push_back(SourceGapProofPiece{
          piece.begin, piece.end, piece.id,
          /*kindOrder=*/0, /*nestingClass=*/0,
          /*absorbedNestingClasses=*/uint64_t{1}, pieceIndex});
    }

    std::string gapReason;
    std::optional<SourceGapProofResult> gapProof =
        proveSourceGapWithIndexedTrivia(preprocessingStructureIndex_,
                                        gapBegin, gapEnd, proofPieces,
                                        &gapReason);
    if (!gapProof) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure rejected recorded conditional gap "
          "source=[{0},{1}): {2}",
          gapBegin, gapEnd, gapReason);
      return false;
    }

    REFOLD_LOG_TRACE("fallback",
                     "TU include-closure preserving recorded conditional gap "
                     "source=[{0},{1}) conditionalGroups={2}",
                     gapBegin, gapEnd,
                     gapProof->outerPiecePayloadIndices.size());
    return true;
  };

  // Return true iff a complete TU conditional group is source-neutral
  // inside a mixed TU/include source envelope.
  //
  // This is the recorded-structure proof for conditional islands that produce
  // no A-side tokens.  Unlike the literal textual #if/#endif scanner, it does
  // not try to evaluate the condition.  The producer has already recorded the
  // selected arm and the arm token spans; this proof only checks that replaying
  // the complete group verbatim is a neutral source gap.  Macro invocations in
  // conditional-control lines are therefore allowed because they remain inside
  // the preserved group.  Recorded artifacts in arm bodies still fail closed:
  // those may carry source tokens, macro state, include effects, or pragmas
  // that require their own proof.
  auto conditionalGroupIsConsumableZeroTokenGap =
      [&](const RefoldModel::CondGroup &group, uint64_t begin,
          uint64_t end) -> bool {
    auto includeIsNeutral = [&](const RefoldModel::IncludeItem &inc) {
      return (!inc.cover.IsValid() && !includeHasRecordedSideEffects(inc)) ||
             includeIsPreservableConditionalStateInclude(inc);
    };
    NeutralConditionalIslandContext islandContext{
        /*requireGroupBeginAtLineStart=*/false,
        NeutralConditionalArmSpanMode::SelectedArmsOnly};
    return RefoldSourceNeutralityProof::ConditionalGroupIsNeutralIsland(
        tuSourceNeutrality, group, begin, end, islandContext, includeIsNeutral);
  };

  // Prove that a source gap inside a replacement envelope consists only of
  // lexical trivia plus complete source-neutral artifacts: zero-token include
  // trees, source-neutral zero-token macro invocation trees, and TU-spelled
  // macro-state directives whose later liveness obligations are handled by the
  // macro-state repair pass.
  //
  // These artifacts have no PP tokens, so token overlap alone cannot make them
  // part of the touched hunk.  If they lie physically between required source
  // pieces of a proved mixed closure, however, they are inside the source
  // interval being replaced and may be consumed with the B-side material.
  //
  // The conditional-control proof is deliberately mode-sensitive.  Mixed
  // whole-envelope closures may consume complete TU conditional groups because
  // their surrounding source interval is itself replaced.  Pure include-closure
  // inter-include gaps, however, project include material and preserve inert
  // source gaps; they must not delete an otherwise-preservable empty #if/#endif
  // island just because it has no PP tokens.
  auto gapIsConsumableZeroTokenSourceClosure =
      [&](uint64_t gapBegin, uint64_t gapEnd,
          bool allowTUConditionalControl) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    struct GapPiece {
      uint64_t begin;
      uint64_t end;
      uint64_t id;
      StringRef kind;
    };

    SmallVector<GapPiece, 8> pieces;
    for (const auto &inc : model_.GetIncludes()) {
      if (!paths_.PathsEqual(inc.sitePath, tuPath) || includeIsTouched(inc))
        continue;
      if (inc.cover.IsValid() || includeHasRecordedSideEffects(inc))
        continue;
      std::optional<std::pair<uint64_t, uint64_t>> exactRange =
          findSourceGapProducerInterval(
              preprocessingStructureIndex_,
              PreprocessingStructureModelKind::IncludeDirective, inc.id);
      if (exactRange && gapBegin <= exactRange->first &&
          exactRange->second <= gapEnd) {
        pieces.push_back(
            {exactRange->first, exactRange->second, inc.id, "include"});
      }
    }

    for (const auto &m : model_.GetMacroInvocations())
      if (macroInvocationIsConsumableZeroTokenGap(m, gapBegin, gapEnd))
        pieces.push_back({*m.invB, *m.invE, m.id, "macro"});

    for (const auto &directive : model_.GetMacroDirectives())
      if (std::optional<MacroDirectiveSourceInterval> interval =
              macroDirectiveIsConsumableStateGap(directive, gapBegin, gapEnd))
        pieces.push_back(
            {interval->begin, interval->end, directive.id, "macro-directive"});

    if (allowTUConditionalControl)
      for (const auto &group : model_.GetConds())
        if (conditionalGroupIsConsumableZeroTokenGap(group, gapBegin,
                                                       gapEnd)) {
          std::optional<std::pair<uint64_t, uint64_t>> exactRange =
              findSourceGapConditionalGroupRange(
                  preprocessingStructureIndex_, group.id);
          if (exactRange && gapBegin <= exactRange->first &&
              exactRange->second <= gapEnd) {
            pieces.push_back({exactRange->first, exactRange->second, group.id,
                              "conditional"});
          }
        }

    uint64_t scanCursor = gapBegin;
    uint64_t includeCount = 0;
    uint64_t macroCount = 0;
    uint64_t macroDirectiveCount = 0;
    uint64_t conditionalGroupCount = 0;
    uint64_t conditionalDirectiveCount = 0;
    unsigned conditionalDepth = 0;

    // Scan source bytes that are not covered by recorded zero-token artifacts.
    // These interstitial bytes may contain ordinary lexical trivia and literal
    // conditional-control directives.  The conditional depth is carried across
    // consumed artifacts, which is what admits shapes such as:
    //
    //   #if 1
    //   #define GAP_VALUE 99
    //   #endif
    //
    // when the #define line is itself a proved consumable macro-state artifact.
    bool atLineStart = stringutils::beginsLineAfterWs(tuBytes, gapBegin);
    auto scanNeutralControlTrivia = [&](uint64_t begin,
                                        uint64_t limit) -> bool {
      scanCursor = begin;
      while (scanCursor < limit) {
        char ch = tuBytes[scanCursor];
        if (stringutils::isWs(ch)) {
          atLineStart = ch == '\n' || ch == '\r';
          ++scanCursor;
          continue;
        }

        if (ch == '/') {
          const uint64_t before = scanCursor;
          StringRef rest = tuBytes.drop_front(scanCursor);
          if (rest.starts_with("/*")) {
            scanCursor += 2;
            bool closed = false;
            while (scanCursor + 1 < limit) {
              if (tuBytes[scanCursor] == '*' &&
                  tuBytes[scanCursor + 1] == '/') {
                scanCursor += 2;
                closed = true;
                break;
              }
              ++scanCursor;
            }
            if (!closed)
              return false;
            StringRef skipped = tuBytes.slice(before, scanCursor);
            atLineStart = skipped.ends_with("\n") || skipped.ends_with("\r");
            continue;
          }

          if (rest.starts_with("//")) {
            scanCursor += 2;
            while (scanCursor < limit && tuBytes[scanCursor] != '\n' &&
                   tuBytes[scanCursor] != '\r')
              ++scanCursor;

            // A line comment may end at the end of the whole gap, but it may
            // not run into a following recorded artifact.  In that case the
            // artifact's spelling would be inside the comment, contradicting
            // the proof that it is an independently recorded source artifact.
            if (scanCursor >= limit)
              return limit == gapEnd;

            ++scanCursor;
            atLineStart = true;
            continue;
          }
        }

        // Literal conditional-control directives are source-neutral only at a
        // physical directive boundary.  Any other directive or token spelling
        // is real source that this closure has not proved safe to erase.
        if (ch != '#' || !atLineStart)
          return false;

        uint64_t lineEnd = scanCursor;
        while (lineEnd < limit && tuBytes[lineEnd] != '\n')
          ++lineEnd;
        if (lineEnd < limit)
          ++lineEnd;

        if (!allowTUConditionalControl ||
            !parseLiteralEmptyConditionalDirectiveLine(
                tuBytes.slice(scanCursor, lineEnd), conditionalDepth)) {
          if (const RefoldModel::PragmaDirective *pragma =
                  findTUPragmaOnSourceLine(scanCursor, lineEnd)) {
            REFOLD_LOG_TRACE(
                "fallback",
                "TU/include closure rejected: source gap [{0},{1}) contains "
                "non-consumable TU pragma id={2} site=[{3},{4}) text='{5}'",
                gapBegin, gapEnd, pragma->id, pragma->siteB, pragma->siteE,
                stringutils::showWsWithClip(pragma->text, 120));
          }
          return false;
        }

        ++conditionalDirectiveCount;
        scanCursor = lineEnd;
        atLineStart = true;
      }
      return scanCursor == limit;
    };

    if (pieces.empty())
      return false;

    enum : uint32_t {
      ConditionalPieceClass = 0,
      IncludePieceClass = 1,
      MacroPieceClass = 2,
      MacroDirectivePieceClass = 3,
    };

    SmallVector<SourceGapProofPiece, 8> proofPieces;
    proofPieces.reserve(pieces.size());
    for (size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
      const GapPiece &piece = pieces[pieceIndex];
      uint32_t pieceClass = MacroDirectivePieceClass;
      uint64_t absorbedClasses = 0;
      if (piece.kind == "conditional") {
        pieceClass = ConditionalPieceClass;
        absorbedClasses = uint64_t{1} << ConditionalPieceClass;
      } else if (piece.kind == "include") {
        pieceClass = IncludePieceClass;
      } else if (piece.kind == "macro") {
        pieceClass = MacroPieceClass;
        absorbedClasses = uint64_t{1} << MacroPieceClass;
      }
      proofPieces.push_back(SourceGapProofPiece{
          piece.begin, piece.end, piece.id, pieceClass, pieceClass,
          absorbedClasses, pieceIndex});
    }

    const uint64_t uncoveredConditionalKinds =
        allowTUConditionalControl ? sourceGapConditionalDirectiveKindMask() : 0;

    std::string gapReason;
    std::optional<SourceGapProofResult> gapProof = proveSourceGapWithPolicy(
        preprocessingStructureIndex_, gapBegin, gapEnd, proofPieces,
        [&](uint64_t begin, uint64_t end) {
          return scanNeutralControlTrivia(begin, end);
        },
        [&](size_t payloadIndex) {
          const GapPiece &piece = pieces[payloadIndex];

          // The artifact itself has already been independently proved
          // zero-token and complete. Consume it as one opaque source-neutral
          // piece, then continue scanning surrounding control/trivia bytes at
          // the same conditional depth.
          StringRef pieceBytes = tuBytes.slice(piece.begin, piece.end);
          atLineStart =
              pieceBytes.ends_with("\n") || pieceBytes.ends_with("\r");
          if (piece.kind == "include")
            ++includeCount;
          else if (piece.kind == "macro")
            ++macroCount;
          else if (piece.kind == "macro-directive")
            ++macroDirectiveCount;
          else
            ++conditionalGroupCount;
        },
        uncoveredConditionalKinds, &gapReason);
    if (!gapProof || conditionalDepth != 0) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected zero-token source gap "
          "source=[{0},{1}): {2}",
          gapBegin, gapEnd,
          gapProof ? StringRef("unbalanced literal conditional controls")
                   : StringRef(gapReason));
      return false;
    }

    REFOLD_LOG_TRACE(
        "fallback",
        "TU/include closure consuming zero-token source gap "
        "source=[{0},{1}) includes={2} macros={3} macroDirectives={4} "
        "conditionalGroups={5} conditionalDirectives={6}",
        gapBegin, gapEnd, includeCount, macroCount, macroDirectiveCount,
        conditionalGroupCount, conditionalDirectiveCount);
    return true;
  };

  struct TUPreservedGapPiece {
    enum class Kind {
      ZeroTokenMacroInvocation,
      ZeroTokenConditionalGroup,
      BalancedPragmaStateIsland
    };

    Kind kind = Kind::ZeroTokenMacroInvocation;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t id = 0;
  };

  auto preservedTUGapPieceText = [&](const TUPreservedGapPiece &piece) {
    if (piece.end <= tuBytes.size() && piece.begin <= piece.end)
      return tuBytes.slice(piece.begin, piece.end).str();
    return std::string();
  };

  auto collectPreservableZeroTokenGapPieces =
      [&](uint64_t gapBegin, uint64_t gapEnd,
          SmallVectorImpl<TUPreservedGapPiece> &out) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    struct GapPiece {
      uint64_t begin;
      uint64_t end;
      uint64_t id;
      TUPreservedGapPiece::Kind kind;
    };

    SmallVector<GapPiece, 8> pieces;
    for (const auto &m : model_.GetMacroInvocations()) {
      if (macroInvocationIsConsumableZeroTokenGap(m, gapBegin, gapEnd)) {
        pieces.push_back({*m.invB, *m.invE, m.id,
                          TUPreservedGapPiece::Kind::ZeroTokenMacroInvocation});
      }
    }

    for (const auto &group : model_.GetConds()) {
      if (conditionalGroupIsConsumableZeroTokenGap(group, gapBegin, gapEnd)) {
        std::optional<std::pair<uint64_t, uint64_t>> exactRange =
            findSourceGapConditionalGroupRange(preprocessingStructureIndex_,
                                               group.id);
        if (exactRange && gapBegin <= exactRange->first &&
            exactRange->second <= gapEnd) {
          pieces.push_back(
              {exactRange->first, exactRange->second, group.id,
               TUPreservedGapPiece::Kind::ZeroTokenConditionalGroup});
        }
      }
    }

    // Pragma/state proof: a complete diagnostic push/settings/pop sequence that
    // is wholly inside this owner gap and crosses only trivia has identity net
    // state at both boundaries.  Preserve the original pragma bytes as an
    // explicit gap piece instead of treating the gap as opaque trivia; unknown
    // or unbalanced pragmas remain non-consumable and will still force the
    // ordinary fail-closed path below.
    SmallVector<BalancedDiagnosticPragmaStateIsland, 4> pragmaIslands;
    collectBalancedDiagnosticPragmaStateIslands(
        model_, tuBytes, gapBegin, gapEnd,
        [&](const RefoldModel::PragmaDirective &pragma) {
          return paths_.PathsEqual(pragma.sitePath, tuPath);
        },
        pragmaIslands, lexLang_);
    for (const BalancedDiagnosticPragmaStateIsland &island : pragmaIslands) {
      pieces.push_back({island.begin, island.end, island.id,
                        TUPreservedGapPiece::Kind::BalancedPragmaStateIsland});
    }

    if (pieces.empty())
      return false;

    SmallVector<SourceGapProofPiece, 8> proofPieces;
    proofPieces.reserve(pieces.size());
    for (size_t pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
      const GapPiece &piece = pieces[pieceIndex];
      const uint32_t pieceClass = static_cast<uint32_t>(piece.kind);
      uint64_t absorbedClasses = 0;
      if (piece.kind ==
          TUPreservedGapPiece::Kind::ZeroTokenMacroInvocation) {
        absorbedClasses = uint64_t{1} << pieceClass;
      } else if (piece.kind ==
                 TUPreservedGapPiece::Kind::ZeroTokenConditionalGroup) {
        // The independently proved complete conditional island owns nested
        // macro invocations, nested conditional records, and balanced pragma
        // islands in its source bytes.
        absorbedClasses =
            (uint64_t{1}
             << static_cast<uint32_t>(
                    TUPreservedGapPiece::Kind::ZeroTokenMacroInvocation)) |
            (uint64_t{1}
             << static_cast<uint32_t>(
                    TUPreservedGapPiece::Kind::ZeroTokenConditionalGroup)) |
            (uint64_t{1}
             << static_cast<uint32_t>(
                    TUPreservedGapPiece::Kind::BalancedPragmaStateIsland));
      }
      proofPieces.push_back(SourceGapProofPiece{
          piece.begin, piece.end, piece.id, pieceClass, pieceClass,
          absorbedClasses, pieceIndex});
    }

    std::string gapReason;
    std::optional<SourceGapProofResult> gapProof =
        proveSourceGapWithIndexedTrivia(preprocessingStructureIndex_,
                                        gapBegin, gapEnd, proofPieces,
                                        &gapReason);
    if (!gapProof) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected preserved zero-token gap "
          "source=[{0},{1}): {2}",
          gapBegin, gapEnd, gapReason);
      return false;
    }

    SmallVector<TUPreservedGapPiece, 8> accepted;
    for (size_t payloadIndex : gapProof->outerPiecePayloadIndices) {
      const GapPiece &piece = pieces[payloadIndex];
      accepted.push_back({piece.kind, piece.begin, piece.end, piece.id});
    }

    out.append(accepted.begin(), accepted.end());
    REFOLD_LOG_TRACE("fallback",
                     "TU/include closure preserving zero-token source gap "
                     "source=[{0},{1}) pieces={2}",
                     gapBegin, gapEnd, accepted.size());
    return true;
  };

  // One TU directive that a closure preserves at its original position inside
  // the replacement instead of consuming or relocating it.
  //
  // `aFrontier` is the producer-backed A-token index the directive sits at; the
  // B payload is cut there so the directive keeps the same position relative to
  // surviving material.
  //
  // Two directive kinds qualify, for the same reason.  A pragma's state is not
  // identity and cannot be moved, and a zero-token `#include` whose header owns
  // such a pragma carries that state transitively: consuming either would drop
  // it.  Preserving the directive keeps the transition at its original
  // position, which is sound where deleting it is not -- and strictly better
  // than refusing the closure, since refusing surrenders every directive in the
  // translation unit rather than this one.
  struct TUPositionPreservedDirective {
    enum class Kind { Pragma, Include, MacroDirective };

    Kind kind = Kind::Pragma;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t id = 0;
    uint64_t aFrontier = 0;
    /// Every state effect this directive establishes, for placing edited
    /// material the alignment cannot place.  StringRefs point into `tuBytes`
    /// for a pragma, or into the model's pragma text for an include.
    SmallVector<PragmaClassification, 2> classifications;
    /// Macro definitions a preserved `#define`/`#undef` binds.  Empty for the
    /// other kinds.  Each binding points into the model's directive record.
    SmallVector<MacroStateBinding, 2> macroBindings;
  };

  // Producer coordinate surfaces for projecting a tokenless TU directive onto
  // the A stream.  They mirror the direct-TU surfaces the alignment's
  // protected-boundary collector projects against: tokmap entries spelled by
  // this TU, and the include occurrences this TU entered directly.  Both are
  // run constants, so they are built once, but only on first use: a closure gap
  // that needs them is rare and building them walks the whole tokmap.
  bool tuTokenlessProjectionSurfacesReady = false;
  SmallVector<const RefoldModel::TokMapEntry *, 32> tuMappedATokens;
  SmallVector<const RefoldModel::IncludeItem *, 8> tuChildIncludes;
  auto ensureTUTokenlessProjectionSurfaces = [&]() {
    if (tuTokenlessProjectionSurfacesReady)
      return;
    tuTokenlessProjectionSurfacesReady = true;
    for (const auto &entry : model_.GetTokmap()) {
      if (entry.pp < aToks_.size() && paths_.PathsEqual(entry.file, tuPath))
        tuMappedATokens.push_back(&entry);
    }
    for (const auto &inc : model_.GetIncludes()) {
      if (!inc.parent && inc.cover.IsValid() &&
          inc.cover.end <= aToks_.size() &&
          paths_.PathsEqual(inc.sitePath, tuPath))
        tuChildIncludes.push_back(&inc);
    }
  };

  // Prove that an owner gap is exactly trivia plus complete TU pragma
  // directives, and project each of those directives onto the A stream.
  //
  // A pragma is neither consumable nor relocatable: its net state is not
  // identity, so deleting it drops a state transition and moving it changes
  // where that transition happens.  The remaining sound realization is to keep
  // it in place and split the replacement's B payload around it, which needs
  // exactly one extra fact per directive -- the A-token frontier it sits at.
  // The projection supplies that fact from producer coordinates and fails
  // closed when the directive is not tokenless or its position is not
  // determined; the gap proof separately guarantees that no other protected
  // structure hides in the same gap.
  auto collectPositionPreservedTUDirectiveGapPieces =
      [&](uint64_t gapBegin, uint64_t gapEnd,
          SmallVectorImpl<TUPositionPreservedDirective> &out) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    // Recover the exact directive extents first.  The indexed lexical extent,
    // not the recorded site, is what gets copied verbatim: the index owns the
    // complete logical directive line.
    SmallVector<TUPositionPreservedDirective, 4> pragmas;
    for (const auto &pragma : model_.GetPragmas()) {
      if (!paths_.PathsEqual(pragma.sitePath, tuPath))
        continue;
      if (pragma.siteB >= pragma.siteE || pragma.siteB < gapBegin ||
          gapEnd < pragma.siteE)
        continue;

      // Only a directive-spelled pragma owns a complete logical line that can
      // be copied verbatim.  A `_Pragma("...")` operator is an expression whose
      // bytes may live inside a macro replacement list, so re-emitting them at
      // a derived split would move source that belongs to another owner.
      if (pragma.viaPragmaOperator) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure cannot preserve pragma id={0} in place: "
            "operator-spelled pragmas have no preservable directive line",
            pragma.id);
        return false;
      }

      std::optional<std::pair<uint64_t, uint64_t>> exactRange =
          findSourceGapProducerInterval(
              preprocessingStructureIndex_,
              PreprocessingStructureModelKind::PragmaDirective, pragma.id);
      if (!exactRange || exactRange->first >= exactRange->second ||
          exactRange->first < gapBegin || gapEnd < exactRange->second) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure cannot preserve TU pragma id={0} in place: "
            "no exact indexed directive interval inside gap [{1},{2})",
            pragma.id, gapBegin, gapEnd);
        return false;
      }

      SmallVector<PragmaClassification, 2> classifications;
      classifications.push_back(classifyPragmaDirective(
          tuBytes.slice(exactRange->first, exactRange->second)));
      pragmas.push_back({TUPositionPreservedDirective::Kind::Pragma,
                         exactRange->first, exactRange->second, pragma.id,
                         /*aFrontier=*/0, std::move(classifications),
                         /*macroBindings=*/{}});
    }

    // A macro directive in the gap is preserved on the same terms.  It is
    // tokenless and it is meaningful source, so consuming it would drop a
    // macro-state transition the gap theorem would otherwise report as an
    // unowned protected interval -- which is what refused the whole closure
    // before, costing every directive in the translation unit to avoid
    // dropping this one.
    for (const auto &directive : model_.GetMacroDirectives()) {
      if (!directive.IsMacroStateDirective())
        continue;
      if (!paths_.PathsEqual(directive.sitePath, tuPath))
        continue;
      if (directive.ownerIncludeId)
        continue;

      std::optional<std::pair<uint64_t, uint64_t>> exactRange =
          findSourceGapProducerInterval(
              preprocessingStructureIndex_,
              PreprocessingStructureModelKind::MacroDirective, directive.id);
      if (!exactRange || exactRange->first >= exactRange->second)
        continue;
      if (exactRange->first < gapBegin || gapEnd < exactRange->second)
        continue;

      SmallVector<MacroStateBinding, 2> macroBindings;
      if (!directive.name.empty())
        macroBindings.push_back(MacroStateBinding{directive.name, &directive});

      pragmas.push_back({TUPositionPreservedDirective::Kind::MacroDirective,
                         exactRange->first, exactRange->second, directive.id,
                         /*aFrontier=*/0,
                         /*classifications=*/{}, std::move(macroBindings)});
    }

    // A zero-token include whose header owns non-consumable state is the same
    // problem one level down: the state is not the include's own text, but
    // deleting the directive drops it just the same.  Preserve the directive
    // and the header is re-entered at its original position, which re-runs
    // every transition it owns exactly where it ran before.  This is only for
    // includes the closure would otherwise have to absorb: a touched include is
    // part of the realized material, and one that produces tokens is not
    // tokenless and has no frontier.
    for (const auto &inc : model_.GetIncludes()) {
      if (!paths_.PathsEqual(inc.sitePath, tuPath) || inc.parent)
        continue;
      if (inc.siteB >= inc.siteE || inc.siteB < gapBegin || gapEnd < inc.siteE)
        continue;
      if (includeIsTouched(inc) || inc.cover.IsValid())
        continue;
      if (!includeHasRecordedSideEffects(inc))
        continue;

      std::optional<std::pair<uint64_t, uint64_t>> exactRange =
          findSourceGapProducerInterval(
              preprocessingStructureIndex_,
              PreprocessingStructureModelKind::IncludeDirective, inc.id);
      if (!exactRange || exactRange->first >= exactRange->second ||
          exactRange->first < gapBegin || gapEnd < exactRange->second) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure cannot preserve include id={0} in place: no "
            "exact indexed directive interval inside gap [{1},{2})",
            inc.id, gapBegin, gapEnd);
        return false;
      }

      // The header's own pragmas are the state a payload could observe by
      // landing on the far side of the preserved directive.  An include whose
      // header records none is still preserved; it then has no classification
      // and any ambiguous payload beside it fails closed.
      SmallVector<PragmaClassification, 2> classifications;
      if (inc.resolvedPath)
        for (const auto &pragma : model_.GetPragmas())
          if (paths_.PathsEqual(pragma.sitePath, *inc.resolvedPath))
            classifications.push_back(classifyPragmaDirective(pragma.text));

      pragmas.push_back({TUPositionPreservedDirective::Kind::Include,
                         exactRange->first, exactRange->second, inc.id,
                         /*aFrontier=*/0, std::move(classifications),
                         /*macroBindings=*/{}});
    }

    if (pragmas.empty())
      return false;

    llvm::sort(pragmas, [](const TUPositionPreservedDirective &lhs,
                           const TUPositionPreservedDirective &rhs) {
      if (lhs.begin != rhs.begin)
        return lhs.begin < rhs.begin;
      return lhs.id < rhs.id;
    });

    ensureTUTokenlessProjectionSurfaces();
    for (TUPositionPreservedDirective &piece : pragmas) {
      std::optional<uint64_t> aFrontier =
          projectTokenlessSourceIntervalToATokenFrontier(
              tuMappedATokens, tuChildIncludes, aToks_.size(), piece.begin,
              piece.end);
      if (!aFrontier) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure cannot preserve TU pragma id={0} in place: "
            "source=[{1},{2}) has no unique A-token frontier",
            piece.id, piece.begin, piece.end);
        return false;
      }
      piece.aFrontier = *aFrontier;
    }

    // Submit the directives as opaque preserved pieces so the shared gap
    // theorem proves the complete byte coverage of the gap.  Every protected
    // interval in the gap must be one of these pieces: an unrelated directive
    // hiding beside the pragma is not preserved by this composition and fails
    // closed here.
    enum : uint32_t { PragmaPieceClass = 0 };
    SmallVector<SourceGapProofPiece, 4> proofPieces;
    proofPieces.reserve(pragmas.size());
    for (size_t pieceIndex = 0; pieceIndex < pragmas.size(); ++pieceIndex) {
      const TUPositionPreservedDirective &piece = pragmas[pieceIndex];
      proofPieces.push_back(SourceGapProofPiece{
          piece.begin, piece.end, piece.id, PragmaPieceClass, PragmaPieceClass,
          0, pieceIndex});
    }

    std::string gapReason;
    std::optional<SourceGapProofResult> gapProof =
        proveSourceGapWithIndexedTrivia(preprocessingStructureIndex_, gapBegin,
                                        gapEnd, proofPieces, &gapReason);
    if (!gapProof) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected position-preserved TU pragma gap "
          "source=[{0},{1}): {2}",
          gapBegin, gapEnd, gapReason);
      return false;
    }
    if (gapProof->outerPiecePayloadIndices.size() != pragmas.size()) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected position-preserved TU pragma gap "
          "source=[{0},{1}): {2} of {3} directives survived normalization",
          gapBegin, gapEnd, gapProof->outerPiecePayloadIndices.size(),
          pragmas.size());
      return false;
    }

    for (size_t payloadIndex : gapProof->outerPiecePayloadIndices) {
      const TUPositionPreservedDirective &piece = pragmas[payloadIndex];
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure preserving TU directive id={0} in place "
          "source=[{1},{2}) aFrontier={3} gap=[{4},{5})",
          piece.id, piece.begin, piece.end, piece.aFrontier, gapBegin, gapEnd);
      out.push_back(piece);
    }
    return true;
  };

  std::string mixedPreservedConditionalControlTrivia;
  SmallVector<TUPositionPreservedDirective, 4> mixedPositionPreservedTUDirectives;
  SmallVector<TUPreservedGapPiece, 8> mixedPreservedZeroTokenGapPieces;
  SmallVector<std::pair<uint64_t, uint64_t>, 4>
      mixedPreservedSourceLineDirectiveGapPieces;
  std::optional<SourceLineDirectiveGapResume> mixedSourceLineDirectiveResume;

  auto lineDirectiveStartsAtPrefix = [&](uint64_t pos) -> bool {
    if (pos >= tuBytes.size())
      return true;
    if (stringutils::isBOL(tuBytes, static_cast<size_t>(pos)))
      return true;

    size_t lineStart = static_cast<size_t>(pos);
    while (lineStart > 0 && tuBytes[lineStart - 1] != '\n')
      --lineStart;
    return stringutils::isIndentOnly(tuBytes, lineStart,
                                     static_cast<size_t>(pos));
  };

  auto canStartLineDirectiveWithOptionalLeadingNewline =
      [&](uint64_t pos) -> bool {
    if (lineDirectiveStartsAtPrefix(pos))
      return true;

    // A delete-only closure may need to synthesize a leading newline before the
    // replacement-local #line directive. Reject the only local case where that
    // newline would be swallowed as a line splice instead of starting a fresh
    // preprocessing directive.
    return pos == 0 || tuBytes[pos - 1] != '\\';
  };

  // Prove a mixed TU/include closure.
  //
  // This handles a single replacement hunk whose A-side tokens are split
  // between ordinary TU spelling and the complete expansion of a contiguous
  // top-level include run, for example:
  //
  //   int arr[] = { 1,
  //   #include "two.inc"
  //   };
  //
  // rewritten from preprocessed tokens:
  //
  //   { 1, 2 }  ->  { 3 }
  //
  // Replacing only the TU-owned bytes for "1," is unsound: the include
  // directive would remain in the source and replay its old "2" token.  The
  // only source realization we accept here is therefore one TU byte interval
  // that spans all TU tokens consumed by the hunk and all touched include
  // directive sites.
  if (mixedTUIncludeClosure) {
    materialBeginA = closureBegin;
    materialEndA = closureEnd;

    // Collect the source intervals that must be consumed by the closure:
    //   * every touched include directive site, and
    //   * every TU-owned token in the widened A token interval.
    //
    // Later checks prove these pieces merge into one safe TU replacement span
    // with only whitespace between pieces and no unrelated artifacts absorbed.
    SmallVector<std::pair<uint64_t, uint64_t>, 16> sourcePieces;
    sourcePieces.reserve(touched.size() + (closureEnd - closureBegin));
    for (const RefoldModel::IncludeItem *inc : touched)
      sourcePieces.push_back({inc->siteB, inc->siteE});

    bool sawTUToken = false;
    const auto &tokmapByPP = model_.GetTokmapByPP();
    for (uint64_t pp = closureBegin; pp < closureEnd; ++pp) {
      auto it = tokmapByPP.find(pp);
      if (it == tokmapByPP.end())
        continue;

      const auto &entry = it->second;
      if (paths_.PathsEqual(entry.file, tuPath)) {
        sourcePieces.push_back({entry.b, entry.e});
        sawTUToken = true;
        continue;
      }

      // Any non-TU token in the closure must be explained by one of the touched
      // include covers.  Otherwise this is not a TU/include closure; it would
      // be a mixed-owner edit involving some third artifact that this proof
      // does not know how to realize soundly.
      if (!tokenCoveredByTouchedInclude(pp)) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: A token {0} maps to file={1} "
            "outside the touched include run A=[{2},{3})",
            pp, entry.file, coverBegin, coverEnd);
        return std::nullopt;
      }
    }

    auto ppSpanInsideMaterial = [&](const RefoldModel::PPSpan &span) -> bool {
      if (!span.IsValid() || span.begin >= span.end)
        return true;
      return materialBeginA <= span.begin && span.end <= materialEndA;
    };

    auto armIsSameOrNestedUnder = [&](const RefoldModel::ArmRef &candidate,
                                      uint64_t ancestorArmId) -> bool {
      if (!candidate.group || !candidate.arm)
        return false;
      if (candidate.arm->id == ancestorArmId)
        return true;

      // `RefoldModel::FindArmRefAtPP()` intentionally returns the innermost
      // selected arm for a PP token.  When deciding whether an outer selected
      // arm is completely consumed, tokens owned by nested selected arms still
      // belong to the outer arm's effective material.  Walk parent-arm links
      // from the innermost group back toward the TU and accept any descendant
      // of the queried arm.
      const RefoldModel::CondGroup *group = candidate.group;
      while (group && group->parentArmId) {
        if (*group->parentArmId == ancestorArmId)
          return true;
        std::optional<RefoldModel::ArmRef> parent =
            model_.GetArmRefById(*group->parentArmId);
        if (!parent || !parent->group)
          break;
        group = parent->group;
      }
      return false;
    };

    auto selectedArmEffectiveMaterialInside =
        [&](const RefoldModel::CondArm &arm) -> bool {
      bool sawEffectiveMaterial = false;
      for (uint64_t pp = 0; pp < aToks_.size(); ++pp) {
        std::optional<RefoldModel::ArmRef> owner = model_.FindArmRefAtPP(pp);
        if (!owner || !armIsSameOrNestedUnder(*owner, arm.id))
          continue;

        sawEffectiveMaterial = true;
        if (pp < materialBeginA || pp >= materialEndA)
          return false;
      }

      // Prefer the effective ownership proof when it found material.  For older
      // maps that cannot answer PP ownership, fall back to the recorded direct
      // span so existing exact whole-arm closures keep their previous domain.
      if (sawEffectiveMaterial)
        return true;
      return arm.span && arm.span->IsValid() &&
             arm.span->begin < arm.span->end && ppSpanInsideMaterial(*arm.span);
    };

    auto macroInvocationMaterialIsConsumed =
        [&](const RefoldModel::MacroInvocation &m,
            std::optional<uint64_t> sourceBeginBound = std::nullopt,
            std::optional<uint64_t> sourceEndBound = std::nullopt) -> bool {
      if (!m.invFile || m.invFile->empty() ||
          !paths_.PathsEqual(*m.invFile, tuPath))
        return false;
      if (!m.invB || !m.invE || *m.invB >= *m.invE || *m.invE > tuBytes.size())
        return false;
      if (sourceBeginBound && *m.invB < *sourceBeginBound)
        return false;
      if (sourceEndBound && *sourceEndBound < *m.invE)
        return false;

      bool sawMaterializedSpan = false;
      auto checkSpan = [&](const RefoldModel::PPSpan &span) -> bool {
        if (!span.IsValid() || span.begin >= span.end)
          return true;
        sawMaterializedSpan = true;
        return ppSpanInsideMaterial(span);
      };

      // A source-bearing macro call may be consumed by this replacement only
      // when every PP span the producer attributes to that invocation is wholly
      // inside the A-side material being replaced. Then the callsite is not an
      // unrelated artifact: it is one of the source pieces whose produced
      // tokens are removed by the hunk. Any span escaping the material range
      // means deleting the call would also delete surviving PP tokens, so
      // reject.
      for (const auto &span : m.spans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.bodySpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.argSpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.stringifySpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.pasteSpans)
        if (!checkSpan(span))
          return false;

      return sawMaterializedSpan;
    };

    // If the consumed A-token material came from inside a TU conditional arm,
    // consume the complete conditional group as a required source piece.  The
    // byte-level gap proof can already consume neutral conditional-control
    // islands whose bodies produce no tokens, but it cannot balance an opening
    // #if in one inter-piece gap against a closing #endif after a real source
    // piece.  A selected arm whose complete PP span is inside the replacement
    // material is different: its directive wrapper is part of the same source
    // interval whose produced tokens are being replaced, so the whole group
    // must be considered a required piece of the closure rather than trivia
    // around neighboring token pieces.
    for (const auto &group : model_.GetConds()) {
      if (!paths_.PathsEqual(group.file, tuPath) || group.parentIncludeId)
        continue;

      bool consumesSelectedArm = false;
      for (const RefoldModel::CondArm &arm : group.arms) {
        if (!arm.selected)
          continue;
        if (selectedArmEffectiveMaterialInside(arm)) {
          consumesSelectedArm = true;
          break;
        }
      }

      if (!consumesSelectedArm)
        continue;
      if (group.groupB >= group.groupE || group.groupE > tuBytes.size()) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: invalid consumed conditional "
            "group id={0} source=[{1},{2}) fileLen={3}",
            group.id, group.groupB, group.groupE, tuBytes.size());
        return std::nullopt;
      }

      sourcePieces.push_back({group.groupB, group.groupE});
    }

    // Collapse any complete source-bearing macro invocation whose produced PP
    // material is wholly replaced by this hunk into one required source piece.
    // Token-level mappings inside macro expansions often point only at the
    // macro name or argument bytes; if we kept only those token pieces, the
    // verifier would later see punctuation from the callsite, such as the
    // parentheses in KEEP(2), as unexplained interstitial source. The complete
    // macro callsite is the actual source artifact whose material is consumed,
    // so prove it once and tile the envelope with that whole interval.
    //
    // A recorded child invocation in a macro replacement list is not such a
    // source artifact. For example, in `#define M() __COUNTER__`, the
    // `__COUNTER__` record has consumed PP material when `M()` is replaced, but
    // its spelling lives inside the preserved #define directive, not at the
    // source site whose bytes should be overwritten. Treating that definition
    // text as a required source piece widens the replacement envelope backward
    // across unrelated TU code and turns a valid owner closure into a terminal
    // fallback. The invariant is therefore: a macro source piece must be a
    // complete consumed invocation site that is not inside a macro-state
    // directive; macro-definition text is preserved or consumed only by the
    // separate macro-directive/gap proofs.
    for (const auto &m : model_.GetMacroInvocations()) {
      if (!macroInvocationMaterialIsConsumed(m))
        continue;
      if (macroTopology_.IsInvocationInsideDefineDirective(m)) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure not using macro id={0} name='{1}' as a "
            "source piece: invocation is inside a #define directive",
            m.id, m.name);
        continue;
      }
      sourcePieces.push_back({*m.invB, *m.invE});
    }

    // The mixed proof is only for hunks that actually consume ordinary TU
    // spelling.  If no TU token was seen, the pure include-closure path is the
    // appropriate proof shape instead.
    if (!sawTUToken) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected: mixed closure A=[{0},{1}) has no "
          "TU-owned source token",
          closureBegin, closureEnd);
      return std::nullopt;
    }

    // Normalize the collected source pieces so the rest of the proof can reason
    // about the single candidate replacement envelope.
    llvm::sort(sourcePieces, [](const auto &lhs, const auto &rhs) {
      if (lhs.first != rhs.first)
        return lhs.first < rhs.first;
      return lhs.second < rhs.second;
    });

    SmallVector<std::pair<uint64_t, uint64_t>, 16> mergedPieces;
    for (const auto &piece : sourcePieces) {
      if (piece.first >= piece.second || piece.second > tuBytes.size()) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: invalid source piece [{0},{1}) "
            "fileLen={2}",
            piece.first, piece.second, tuBytes.size());
        return std::nullopt;
      }
      if (mergedPieces.empty() || piece.first > mergedPieces.back().second) {
        mergedPieces.push_back(piece);
        continue;
      }
      mergedPieces.back().second =
          std::max(mergedPieces.back().second, piece.second);
    }

    // The replacement will cover the whole byte envelope from the first
    // required source piece through the last. `mergedPieces` is already sorted,
    // non-empty, and coalesced by source interval.
    if (mergedPieces.empty()) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected: no normalized source envelope");
      return std::nullopt;
    }
    sourceBegin = mergedPieces.front().first;
    sourceEnd = mergedPieces.back().second;

    auto gapHasMappedTUToken = [&](uint64_t gapBegin, uint64_t gapEnd) -> bool {
      for (const auto &entry : model_.GetTokmap()) {
        if (!paths_.PathsEqual(entry.file, tuPath))
          continue;
        if (entry.b < gapEnd && gapBegin < entry.e)
          return true;
      }
      return false;
    };

    auto gapHasRecordedNonConditionalArtifact = [&](uint64_t gapBegin,
                                                    uint64_t gapEnd) -> bool {
      auto overlapsGap = [&](uint64_t begin, uint64_t end) {
        return begin < gapEnd && gapBegin < end;
      };

      for (const auto &inc : model_.GetIncludes())
        if (paths_.PathsEqual(inc.sitePath, tuPath) &&
            overlapsGap(inc.siteB, inc.siteE))
          return true;
      for (const auto &m : model_.GetMacroInvocations())
        if (m.invFile && paths_.PathsEqual(*m.invFile, tuPath) && m.invB &&
            m.invE && overlapsGap(*m.invB, *m.invE))
          return true;
      for (const auto &directive : model_.GetMacroDirectives())
        if (paths_.PathsEqual(directive.sitePath, tuPath) &&
            overlapsGap(directive.siteB, directive.siteE))
          return true;
      for (const auto &pragma : model_.GetPragmas())
        if (paths_.PathsEqual(pragma.sitePath, tuPath) &&
            overlapsGap(pragma.siteB, pragma.siteE))
          return true;
      return false;
    };

    auto gapIsRecordedConditionalControlTail = [&](uint64_t gapBegin,
                                                   uint64_t gapEnd) -> bool {
      if (!h.isDeleteOnly() || gapBegin >= gapEnd || gapEnd > tuBytes.size())
        return false;
      if (gapHasMappedTUToken(gapBegin, gapEnd) ||
          gapHasRecordedNonConditionalArtifact(gapBegin, gapEnd))
        return false;

      const RefoldModel::CondGroup *owningGroup = nullptr;
      for (const auto &group : model_.GetConds()) {
        if (!paths_.PathsEqual(group.file, tuPath) || group.parentIncludeId)
          continue;
        std::optional<std::pair<uint64_t, uint64_t>> exactRange =
            findSourceGapConditionalGroupRange(preprocessingStructureIndex_,
                                               group.id);
        if (!exactRange || exactRange->first >= gapBegin ||
            gapBegin >= exactRange->second || gapEnd != exactRange->second) {
          continue;
        }
        if (owningGroup)
          return false;
        owningGroup = &group;
      }
      if (!owningGroup)
        return false;

      // This legacy delete-only repair preserves a suffix made solely of
      // closing conditional controls.  Use the shared source-gap theorem to
      // prove exact directive/trivia byte coverage, then retain only the
      // path-specific semantic restriction that every indexed structure is a
      // producer-bound #endif.  No independent line parser or source-offset
      // search remains in the fallback planner.
      std::string gapReason;
      std::optional<SourceGapProofResult> gapProof =
          proveSourceGapWithIndexedStructureAndTrivia(
              preprocessingStructureIndex_, gapBegin, gapEnd, &gapReason);
      if (!gapProof || gapProof->protectedIntervals.empty())
        return false;

      bool sawOwningEndif = false;
      for (const PreprocessingStructureInterval *interval :
           gapProof->protectedIntervals) {
        if (!interval ||
            interval->kind != PreprocessingStructureKind::ConditionalEndif ||
            interval->modelKind !=
                PreprocessingStructureModelKind::ConditionalDirective ||
            !interval->conditionalGroupId) {
          return false;
        }
        if (*interval->conditionalGroupId == owningGroup->id)
          sawOwningEndif = true;
      }
      if (!sawOwningEndif)
        return false;

      REFOLD_LOG_TRACE("fallback",
                       "TU/include closure preserving conditional-control tail "
                       "source=[{0},{1}) group={2}",
                       gapBegin, gapEnd, owningGroup->id);
      return true;
    };

    auto includeIsInsidePreservableZeroTokenConditionalGap =
        [&](const RefoldModel::IncludeItem &inc) -> bool {
      for (const auto &group : model_.GetConds()) {
        if (!conditionalGroupIsConsumableZeroTokenGap(group, sourceBegin,
                                                      sourceEnd))
          continue;
        if (group.groupB <= inc.siteB && inc.siteB < inc.siteE &&
            inc.siteE <= group.groupE)
          return true;
      }
      return false;
    };

    // Return whether an include's directive site lies wholly between two
    // required source pieces rather than overlapping one.  Only such an include
    // is a candidate for position preservation: one overlapping a piece is part
    // of the material the closure realizes.
    auto includeLiesInsideEnvelopeGap =
        [&](const RefoldModel::IncludeItem &inc) -> bool {
      for (size_t idx = 1; idx < mergedPieces.size(); ++idx) {
        const uint64_t gapBegin = mergedPieces[idx - 1].second;
        const uint64_t gapEnd = mergedPieces[idx].first;
        if (gapBegin <= inc.siteB && inc.siteE <= gapEnd)
          return true;
      }
      return false;
    };

    // Do not silently delete any include directive other than the run whose
    // expansion is explicitly part of this closure.  A complete unrelated
    // include with an empty PP cover may be consumed only when it is fully
    // inside the same source envelope and all recorded structure is either
    // absent or limited to source-neutral conditional control, nested
    // zero-token includes, and consumable macro-state directives.  That makes
    // it part of the replaced source interval, not trivia to preserve or
    // reorder around the B-side material.
    for (const auto &inc : model_.GetIncludes()) {
      if (!paths_.PathsEqual(inc.sitePath, tuPath))
        continue;
      if (inc.siteB >= sourceEnd || sourceBegin >= inc.siteE)
        continue;
      std::optional<std::string> sideEffectReason =
          includeRecordedSideEffectReason(inc);

      // A *touched* include is consumed because this closure realizes its
      // expansion, and that accounts for its tokens.  It does not account for
      // preprocessor state the header establishes that outlives the include:
      // `#pragma push_macro` is popped by the translation unit after the
      // include has ended, so consuming the directive deletes the push while
      // the pop survives, and the pop then restores nothing.  Consuming is
      // sound only when every recorded effect is include-local, which is what
      // the side-effect resolver decides.  Otherwise leave the closure
      // unformed so the realization lattice materializes the include instead,
      // where the directive survives where it was written.
      if (includeIsTouched(inc)) {
        if (!includeSubtreeOwnsOutlivingPragma(inc))
          continue;
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: source interval [{0},{1}) would "
            "consume touched include id={2} site=[{3},{4}) whose subtree owns "
            "a pragma outliving it",
            sourceBegin, sourceEnd, inc.id, inc.siteB, inc.siteE);
        return std::nullopt;
      }
      if (!inc.cover.IsValid() && !sideEffectReason &&
          sourceBegin <= inc.siteB && inc.siteE <= sourceEnd)
        continue;
      if (sideEffectReason &&
          includeIsInsidePreservableZeroTokenConditionalGap(inc))
        continue;

      // An include whose header owns non-consumable state need not sink the
      // whole closure.  When it lies in a gap between required source pieces it
      // can be preserved there instead, and the gap proof below decides that.
      // Rejecting here would give up every directive in the translation unit to
      // avoid dropping this one, which is a strictly worse trade.
      if (sideEffectReason && !inc.cover.IsValid() &&
          includeLiesInsideEnvelopeGap(inc)) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure deferring unrelated include id={0} "
            "site=[{1},{2}) to the source-gap proof: {3}",
            inc.id, inc.siteB, inc.siteE, *sideEffectReason);
        continue;
      }

      if (sideEffectReason) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: source interval [{0},{1}) would "
            "absorb unrelated include id={2} site=[{3},{4}) cover=[{5},{6}): "
            "{7}",
            sourceBegin, sourceEnd, inc.id, inc.siteB, inc.siteE,
            inc.cover.begin, inc.cover.end, *sideEffectReason);
      } else {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: source interval [{0},{1}) would "
            "absorb unrelated include id={2} site=[{3},{4}) cover=[{5},{6})",
            sourceBegin, sourceEnd, inc.id, inc.siteB, inc.siteE,
            inc.cover.begin, inc.cover.end);
      }
      return std::nullopt;
    }


    SourceLineDirectiveLogicalLineRewriter sourceLineDirectiveLineRewriter =
        [&](StringRef logicalLine, ArrayRef<uint64_t> sourceOffsets,
            SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver)
        -> std::optional<SourceLineDirectiveLogicalLineRewrite> {
      return rewriteSourceLineDirectiveLogicalLineMacros(
          model_, tuPath, logicalLine, sourceOffsets, paths_, lexLang_,
          std::nullopt, std::move(builtinMacroResolver));
    };

    DenseSet<uint64_t> mixedSourceLineDirectiveMacroIds;
    auto recordSourceLineDirectiveMacroIds = [&](ArrayRef<uint64_t> macroIds) {
      for (uint64_t macroId : macroIds)
        mixedSourceLineDirectiveMacroIds.insert(macroId);
    };

    auto macroInvocationIsInsideConsumableSourceLineDirectiveGap =
        [&](const RefoldModel::MacroInvocation &macro) -> bool {
      if (!macro.invFile || macro.invFile->empty() ||
          !paths_.PathsEqual(*macro.invFile, tuPath))
        return false;
      if (!macro.invB || !macro.invE || *macro.invB >= *macro.invE)
        return false;

      for (size_t idx = 1; idx < mergedPieces.size(); ++idx) {
        const uint64_t gapBegin = mergedPieces[idx - 1].second;
        const uint64_t gapEnd = mergedPieces[idx].first;
        if (*macro.invB < gapBegin || gapEnd < *macro.invE)
          continue;

        SmallVector<uint64_t, 4> acceptedMacroIds;
        if (!computeSourceLineDirectiveGapResume(
                tuBytes, gapBegin, gapEnd, sourceEnd, tuPath,
                sourceLineDirectiveLineRewriter, &acceptedMacroIds, StringRef(),
                !sourceSuffixMayObservePresumedFileSpelling(
                    model_, tuPath, sourceEnd, paths_, tuBytes)))
          continue;

        recordSourceLineDirectiveMacroIds(acceptedMacroIds);
        for (uint64_t macroId : acceptedMacroIds)
          if (macroId == macro.id)
            return true;
      }
      return false;
    };

    auto macroInvocationIsInsidePreservableZeroTokenConditionalGap =
        [&](const RefoldModel::MacroInvocation &macro) -> bool {
      if (!macro.invFile || macro.invFile->empty() ||
          !paths_.PathsEqual(*macro.invFile, tuPath))
        return false;
      if (!macro.invB || !macro.invE || *macro.invB >= *macro.invE)
        return false;

      for (const auto &group : model_.GetConds()) {
        if (!conditionalGroupIsConsumableZeroTokenGap(group, sourceBegin,
                                                      sourceEnd))
          continue;
        if (group.groupB <= *macro.invB && *macro.invB < *macro.invE &&
            *macro.invE <= group.groupE)
          return true;
      }
      return false;
    };

    // Do not delete or partially overwrite unrelated macro invocations.  Four
    // complete-callsite cases are allowed inside the replacement envelope:
    //
    //   * macro-expanded include-target spelling inside a touched include
    //     directive, because the directive site is already a required piece;
    //   * zero-token source-neutral macro gaps, which contributed no PP tokens;
    //   * macro uses in preserved zero-token conditional-control islands;
    //   * source-bearing macro calls whose entire produced PP material is
    //     already inside this hunk's A-side replacement range.
    //
    // The last case is the source-bearing analogue of the zero-token gap: the
    // macro invocation is not being preserved or moved, it is part of the
    // source interval whose materialized tokens the user replaced.
    for (const auto &m : model_.GetMacroInvocations()) {
      if (m.invFile && !m.invFile->empty() &&
          !paths_.PathsEqual(*m.invFile, tuPath))
        continue;
      if (!m.invB || !m.invE)
        continue;
      if (*m.invB < sourceEnd && sourceBegin < *m.invE) {
        if (macroInvocationIsInsideTouchedIncludeDirective(m) ||
            macroInvocationIsConsumableZeroTokenGap(m, sourceBegin,
                                                    sourceEnd) ||
            macroInvocationMaterialIsConsumed(m, sourceBegin, sourceEnd) ||
            mixedSourceLineDirectiveMacroIds.contains(m.id) ||
            macroInvocationIsInsideConsumableSourceLineDirectiveGap(m) ||
            macroInvocationIsInsidePreservableZeroTokenConditionalGap(m))
          continue;
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: source interval [{0},{1}) would "
            "absorb macro invocation id={2} name='{3}' site=[{4},{5})",
            sourceBegin, sourceEnd, m.id, m.name, *m.invB, *m.invE);
        return std::nullopt;
      }
    }

    // The envelope may widen across physical gaps between required pieces, but
    // only across source-neutral control/trivia or complete zero-token source
    // artifacts.  Complete comments and literal empty conditional-control
    // islands are consumed with the replacement for the same reason as
    // whitespace: they contribute no PP tokens and are physically inside the
    // source interval whose A-side material was replaced by the B-side hunk.
    // Any other byte would be meaningful source text that the proof has not
    // justified replacing.
    // The source envelope was already computed from the normalized piece list
    // above.  At this point the only remaining common source-envelope proof
    // step is the inter-piece gap proof; avoid recomputing the same envelope
    // here.
    if (!proveSourceEnvelopeGaps(mergedPieces, [&](uint64_t gapBegin,
                                                   uint64_t gapEnd) {
          StringRef gap = tuBytes.slice(gapBegin, gapEnd);
          const bool preserveConditionalControlTail =
              gapIsRecordedConditionalControlTail(gapBegin, gapEnd);
          if (preserveConditionalControlTail) {
            mixedPreservedConditionalControlTrivia.append(gap.begin(),
                                                          gap.end());
            return true;
          }
          if (gapIsIndexedPreservableIncludeClosureTrivia(gapBegin,
                                                            gapEnd))
            return true;

          SmallVector<TUPreservedGapPiece, 4> preservedZeroTokenPieces;
          if (collectPreservableZeroTokenGapPieces(gapBegin, gapEnd,
                                                   preservedZeroTokenPieces)) {
            mixedPreservedZeroTokenGapPieces.append(
                preservedZeroTokenPieces.begin(),
                preservedZeroTokenPieces.end());
            return true;
          }

          if (gapIsConsumableZeroTokenSourceClosure(
                  gapBegin, gapEnd,
                  /*allowTUConditionalControl=*/true))
            return true;

          SmallVector<uint64_t, 4> acceptedMacroIds;
          if (std::optional<SourceLineDirectiveGapResume> lineResume =
                  computeSourceLineDirectiveGapResume(
                      tuBytes, gapBegin, gapEnd, sourceEnd, tuPath,
                      sourceLineDirectiveLineRewriter, &acceptedMacroIds,
                      StringRef(),
                      !sourceSuffixMayObservePresumedFileSpelling(
                          model_, tuPath, sourceEnd, paths_, tuBytes))) {
            // A source-spelled line-control directive contributes no PP
            // tokens, but it is not disposable trivia.  If the copied
            // suffix has no live line-state observer, preserve the original
            // source directive spelling as source material.  If the suffix
            // does observe line state, emit a local resume that restores the
            // net state at the suffix after the consumed envelope.
            recordSourceLineDirectiveMacroIds(acceptedMacroIds);
            if (!lineControlProof_
                     .OwnerSuffixLineStateObserverDemand(std::nullopt, tuPath,
                                                         sourceEnd)
                     .Any()) {
              mixedPreservedSourceLineDirectiveGapPieces.push_back(
                  {gapBegin, gapEnd});
              REFOLD_LOG_TRACE(
                  "fallback",
                  "TU/include closure preserving source #line spelling "
                  "from gap=[{0},{1})",
                  gapBegin, gapEnd);
            } else {
              mixedSourceLineDirectiveResume = std::move(lineResume);
              REFOLD_LOG_TRACE(
                  "fallback",
                  "TU/include closure preserving source #line state from "
                  "gap=[{0},{1}) resumeLine={2} file={3}",
                  gapBegin, gapEnd,
                  mixedSourceLineDirectiveResume->lineAtResume,
                  mixedSourceLineDirectiveResume->fileSpelling);
            }
            return true;
          }

          // A pragma cannot be consumed or relocated, but it can be kept where
          // it is when the B payload can be split around it.  Try that before
          // the ordinary rejection; the split itself is derived later, once
          // the B material range is known, and refuses there if it is not
          // determined.
          if (collectPositionPreservedTUDirectiveGapPieces(
                  gapBegin, gapEnd, mixedPositionPreservedTUDirectives))
            return true;

          if (std::optional<std::string> pragmaReason =
                  nonConsumableTUPragmaGapReason(gapBegin, gapEnd)) {
            REFOLD_LOG_TRACE("fallback", "TU/include closure rejected: {0}",
                             *pragmaReason);
          } else {
            REFOLD_LOG_TRACE(
                "fallback",
                "TU/include closure rejected: non-trivia TU gap inside "
                "mixed source interval [{0},{1}) gap='{2}'",
                gapBegin, gapEnd, stringutils::showWsWithClip(gap, 120));
          }
          return false;
        }))
      return std::nullopt;

    if (mixedSourceLineDirectiveResume && !lineDirs_.Enabled() &&
        sourceEnd < tuBytes.size()) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected: source #line gap needs enabled "
          "line directives to preserve the untouched suffix");
      return std::nullopt;
    }

    if (lineDirs_.Enabled() && mixedSourceLineDirectiveResume &&
        !lineDirectiveStartsAtPrefix(sourceEnd)) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected: source #line state from consumed "
          "gap cannot be restored before non-BOL suffix at source={0}",
          sourceEnd);
      return std::nullopt;
    }

    if (lineDirs_.Enabled() && mixedSourceLineDirectiveResume &&
        h.isDeleteOnly() &&
        !canStartLineDirectiveWithOptionalLeadingNewline(sourceBegin)) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected: delete-only source #line resume "
          "would be line-spliced at source={0}",
          sourceBegin);
      return std::nullopt;
    }

    REFOLD_LOG_TRACE(
        "fallback",
        "TU/include closure source interval accepted: source=[{0},{1}) "
        "A=[{2},{3}) includeCover=[{4},{5})",
        sourceBegin, sourceEnd, materialBeginA, materialEndA, coverBegin,
        coverEnd);
  }

  if (!exactCover) {
    if (!hunkInsideCover && !mixedTUIncludeClosure) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure rejected: touched include cover A=[{0},{1}) "
          "does not contain hunk A=[{2},{3})",
          coverBegin, coverEnd, h.aStart, h.aEnd);
      return std::nullopt;
    }

    // Widening from the unresolved hunk to the complete closure A-range is
    // admissible only when the widened-away prefix/suffix are observationally
    // unchanged in the token diff. That keeps this as a source-closure proof,
    // not a hidden multi-region merge: any independent token edit or insertion
    // in the extra cover must be handled by the normal partition/lattice path.
    const bool prefixClean =
        !rangeHasForeignTokenDiff(materialBeginA, h.aStart);
    const bool suffixClean = !rangeHasForeignTokenDiff(h.aEnd, materialEndA);
    if (!prefixClean || !suffixClean) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure rejected: widening A=[{0},{1}) around hunk "
          "A=[{2},{3}) would absorb foreign token edits prefixClean={4} "
          "suffixClean={5}",
          coverBegin, coverEnd, h.aStart, h.aEnd, prefixClean ? 1 : 0,
          suffixClean ? 1 : 0);
      return std::nullopt;
    }

    if (sourceTouchesStagedEdit(sourceBegin, sourceEnd)) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure rejected: widened source interval [{0},{1}) "
          "would absorb an already-staged source edit",
          sourceBegin, sourceEnd);
      return std::nullopt;
    }

    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure widening accepted: hunk A=[{0},{1}) widened to "
        "closure A=[{2},{3}) source=[{4},{5})",
        h.aStart, h.aEnd, materialBeginA, materialEndA, sourceBegin, sourceEnd);
  }

  if (sourceBegin >= sourceEnd || sourceEnd > tuBytes.size()) {
    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure rejected: invalid TU source interval [{0},{1}) "
        "fileLen={2}",
        sourceBegin, sourceEnd, tuBytes.size());
    return std::nullopt;
  }

  std::string preservedGapTrivia;
  uint64_t sourceCursor = first->siteB;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->siteB < sourceCursor || inc->siteE > tuBytes.size() ||
        inc->siteB >= inc->siteE) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure rejected: invalid include site for inc#{0} "
          "site=[{1},{2}) cursor={3} fileLen={4}",
          inc->id, inc->siteB, inc->siteE, sourceCursor, tuBytes.size());
      return std::nullopt;
    }

    // Inter-include source bytes are handled differently for pure and mixed
    // closures.  In a pure include closure, the source edit starts at the first
    // include directive and projects include material only, so inert comments
    // or directive-control trivia between adjacent include directives may need
    // to be preserved verbatim.  In a mixed TU/include closure, the earlier
    // whole-envelope verifier has already proved every gap between required
    // source pieces, including bytes between split include runs, and those
    // bytes are consumed by the B-side replacement.  Revalidating or preserving
    // them here would incorrectly require a pure contiguous-include closure.
    if (!mixedTUIncludeClosure && inc->siteB > sourceCursor) {
      StringRef gap = tuBytes.slice(sourceCursor, inc->siteB);
      if (!gap.empty() && stringutils::isWs(gap)) {
        // Retain the historical policy of consuming whitespace-only gaps, but
        // submit their physical byte coverage to the same exact lexical census
        // used by every other source-gap path.
        if (!gapIsIndexedLexerTrivia(sourceCursor, inc->siteB)) {
          REFOLD_LOG_TRACE(
              "fallback",
              "TU include-closure rejected: whitespace gap before inc#{0} "
              "is not an exact indexed trivia interval source=[{1},{2})",
              inc->id, sourceCursor, inc->siteB);
          return std::nullopt;
        }
      } else if (!gap.empty()) {
        if (gapIsConsumableZeroTokenSourceClosure(
                sourceCursor, inc->siteB,
                /*allowTUConditionalControl=*/false)) {
          REFOLD_LOG_TRACE(
              "fallback",
              "TU include-closure consuming zero-token source gap before "
              "inc#{0} gap='{1}'",
              inc->id, stringutils::showWsWithClip(gap, 120));
        } else if (!gapIsIndexedPreservableIncludeClosureTrivia(
                       sourceCursor, inc->siteB) &&
                   !gapIsPreservableRecordedConditionalIncludeClosure(
                       sourceCursor, inc->siteB)) {
          REFOLD_LOG_TRACE(
              "fallback",
              "TU include-closure rejected: non-trivia TU gap between "
              "include directives before inc#{0} gap='{1}'",
              inc->id, stringutils::showWsWithClip(gap, 120));
          return std::nullopt;
        } else {
          preservedGapTrivia.append(gap.begin(), gap.end());
          REFOLD_LOG_TRACE(
              "fallback",
              "TU include-closure preserving inert trivia/control gap before "
              "inc#{0} gap='{1}'",
              inc->id, stringutils::showWsWithClip(gap, 120));
        }
      }
    }

    sourceCursor = inc->siteE;
  }

  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvelope =
      includeInsertionPlanner_.ResolveIncludeRealizationBTokenEnvelope(
          materialBeginA, materialEndA, &evidenceKind);
  if (h.isDeleteOnly() && materialBeginA == h.aStart &&
      materialEndA == h.aEnd) {
    // A delete-only exact closure has no edited B material by definition.  Do
    // not let a neighbor-based cover projection reintroduce adjacent surviving
    // tokens into the replacement; any source-control bytes that must survive
    // are carried explicitly as preserved trivia below.
    bEnvelope = std::make_pair(static_cast<size_t>(h.bStart),
                               static_cast<size_t>(h.bEnd));
    evidenceKind = IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope;
    REFOLD_LOG_TRACE("fallback",
                     "TU/include closure using exact delete hunk B envelope: "
                     "A=[{0},{1}) B=[{2},{3})",
                     materialBeginA, materialEndA, h.bStart, h.bEnd);
  } else if (!bEnvelope && materialBeginA == h.aStart &&
             materialEndA == h.aEnd) {
    bEnvelope = std::make_pair(static_cast<size_t>(h.bStart),
                               static_cast<size_t>(h.bEnd));
    evidenceKind = IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope;
    REFOLD_LOG_TRACE("fallback",
                     "TU/include closure using exact diff hunk B envelope: "
                     "A=[{0},{1}) B=[{2},{3})",
                     materialBeginA, materialEndA, h.bStart, h.bEnd);
  }
  if (!bEnvelope) {
    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure rejected: no canonical-or-consensus B envelope "
        "for A-cover [{0},{1})",
        materialBeginA, materialEndA);
    return std::nullopt;
  }

  auto bMaterialBegin = bEnvelope->first;
  auto bMaterialEnd = bEnvelope->second;

  // Pure include-closure projection can include stable boundary tokens that are
  // outside the include cover, such as the `{` before an include run inside an
  // initializer. The source edit starts at the first include directive, so such
  // outside anchors must not be materialized or they would be duplicated in the
  // TU. A mixed TU/include closure consumes those TU-side boundary tokens as
  // part of the same source interval, so it keeps the full closure envelope.
  if (!mixedTUIncludeClosure) {
    auto bTokenMapsOutsideCover = [&](size_t bTok) -> bool {
      if (bTok >= abTokMapB2A_.size())
        return false;
      const int64_t mappedA = abTokMapB2A_[bTok];
      if (mappedA < 0)
        return false;
      const uint64_t aTok = static_cast<uint64_t>(mappedA);
      return aTok < coverBegin || aTok >= coverEnd;
    };

    while (bMaterialBegin < bMaterialEnd &&
           bTokenMapsOutsideCover(bMaterialBegin))
      ++bMaterialBegin;
    while (bMaterialBegin < bMaterialEnd &&
           bTokenMapsOutsideCover(bMaterialEnd - 1))
      --bMaterialEnd;
  }

  if (h.bStart < bMaterialBegin || h.bEnd > bMaterialEnd) {
    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure rejected: hunk B=[{0},{1}) is outside trimmed "
        "closure B material [{2},{3}) from envelope [{4},{5})",
        h.bStart, h.bEnd, bMaterialBegin, bMaterialEnd, bEnvelope->first,
        bEnvelope->second);
    return std::nullopt;
  }

  // Compose the realized B payload.  Without a preserved in-place directive
  // that is one contiguous B slice; with one, the payload is partitioned at the
  // directive's derived split so the directive keeps its original position
  // relative to the surviving material.
  std::string rawReplacement;
  if (mixedPositionPreservedTUDirectives.empty()) {
    rawReplacement =
        sourceMapper_.SliceBSource(bMaterialBegin, bMaterialEnd).str();
  } else {
    SmallVector<PreservedDirectiveFrontier, 4> preservedFrontiers;
    preservedFrontiers.reserve(mixedPositionPreservedTUDirectives.size());
    for (const TUPositionPreservedDirective &pragma :
         mixedPositionPreservedTUDirectives)
      preservedFrontiers.push_back(
          {pragma.aFrontier, pragma.classifications, pragma.macroBindings});

    std::string splitReason;
    std::optional<SmallVector<size_t, 4>> splits =
        deriveBPayloadSplitsAtATokenFrontiers(
            macroStateProof_, abTokMapB2A_, abTokAnchorProofs_,
            preservedFrontiers,
            bMaterialBegin, bMaterialEnd,
            [&](size_t begin, size_t end) {
              return sourceMapper_.SliceBSource(begin, end);
            },
            lexLang_, &splitReason);
    if (!splits) {
      REFOLD_LOG_TRACE(
          "fallback",
          "TU/include closure rejected: B payload [{0},{1}) cannot be "
          "partitioned around {2} preserved TU pragma(s): {3}",
          bMaterialBegin, bMaterialEnd, mixedPositionPreservedTUDirectives.size(),
          splitReason);
      return std::nullopt;
    }

    // A preserved directive owns its own logical line: it must begin one, and
    // the payload after it must begin a fresh one.  When the directive opens
    // the replacement, the line must start at the source position the
    // replacement is spliced into.  A payload ending in a line splice would
    // swallow the separator instead of ending the line, so that shape stays
    // rejected rather than emitting a glued directive.
    auto startPreservedDirectiveLine = [&](std::string &text) -> bool {
      if (text.empty()) {
        if (lineDirectiveStartsAtPrefix(sourceBegin))
          return true;
        if (!canStartLineDirectiveWithOptionalLeadingNewline(sourceBegin))
          return false;
        text.push_back('\n');
        return true;
      }
      if (text.back() == '\n')
        return true;
      if (text.back() == '\\')
        return false;
      text.push_back('\n');
      return true;
    };

    std::string splitTrace;
    size_t cursor = bMaterialBegin;
    uint64_t previousPragmaEnd = 0;
    for (size_t index = 0; index < mixedPositionPreservedTUDirectives.size();
         ++index) {
      const TUPositionPreservedDirective &pragma =
          mixedPositionPreservedTUDirectives[index];

      // The preserved directives are emitted in the order they were collected,
      // so that order must be their source order.  Gaps are proved left to
      // right and each gap sorts its own directives, but the composition
      // depends on the result rather than assuming it.
      if (pragma.begin < previousPragmaEnd) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: preserved TU pragma id={0} "
            "source=[{1},{2}) is not in source order after byte {3}",
            pragma.id, pragma.begin, pragma.end, previousPragmaEnd);
        return std::nullopt;
      }
      previousPragmaEnd = pragma.end;

      const size_t split = (*splits)[index];
      StringRef leadingPayload = sourceMapper_.SliceBSource(cursor, split);
      rawReplacement.append(leadingPayload.begin(), leadingPayload.end());
      if (!startPreservedDirectiveLine(rawReplacement)) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure rejected: preserved TU pragma id={0} cannot "
            "start a physical line after the realized B payload",
            pragma.id);
        return std::nullopt;
      }

      StringRef pragmaBytes = tuBytes.slice(pragma.begin, pragma.end);
      rawReplacement.append(pragmaBytes.begin(), pragmaBytes.end());
      if (rawReplacement.back() != '\n')
        rawReplacement.push_back('\n');

      if (!splitTrace.empty())
        splitTrace += ",";
      splitTrace += std::to_string(split);
      cursor = split;
    }
    StringRef trailingPayload =
        sourceMapper_.SliceBSource(cursor, bMaterialEnd);
    rawReplacement.append(trailingPayload.begin(), trailingPayload.end());

    REFOLD_LOG_DEBUG(
        "fallback",
        "TU/include closure partitioned B payload [{0},{1}) around {2} "
        "preserved TU directive(s): splits={3} result='{4}'",
        bMaterialBegin, bMaterialEnd, mixedPositionPreservedTUDirectives.size(),
        splitTrace, stringutils::showWsWithClip(rawReplacement, 120));
  }
  std::string replacement = rawReplacement;

  // Delete-only mixed closures may span a conditional-control tail that must
  // remain in the TU even though the source pieces on both sides are consumed.
  // Such bytes are admitted only when they are a complete recorded #endif tail
  // with no mapped PP tokens or unrelated source artifacts in the gap.
  if (!mixedPreservedConditionalControlTrivia.empty()) {
    if (startsWithPreprocessorDirectiveTrivia(
            mixedPreservedConditionalControlTrivia) &&
        !replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    replacement.append(mixedPreservedConditionalControlTrivia.begin(),
                       mixedPreservedConditionalControlTrivia.end());
  }

  // Preserve inert source trivia between touched include directives.  Empty
  // includes admitted by the gap proof above are deliberately not appended
  // here: they lie inside the source envelope replaced by this closure, so the
  // B-side material consumes them along with the adjacent TU/include tokens.
  if (!preservedGapTrivia.empty()) {
    if (startsWithPreprocessorDirectiveTrivia(preservedGapTrivia) &&
        !replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    replacement.append(preservedGapTrivia.begin(), preservedGapTrivia.end());
  }

  // Mixed TU/include closures may span source-neutral macro callsites, complete
  // conditional islands, or balanced pragma-state islands that contributed no
  // normal PP tokens.  These pieces are not stale source: preserving them keeps
  // the refolded source closer to the original while the owner-gap proof above
  // guarantees that replaying them cannot change the B token stream.
  //
  // Preserved pragma directives add one extra ownership choice: they are
  // sideband replay bytes: after frontend normalization, the ordinary B hunk
  // may already contain the exact balanced diagnostic island even though the
  // island is absent from the structural token diff.  In that case the B
  // surface, not the source-gap copier, owns the pragma replay.  Appending the
  // source island here would duplicate the directives and change validation.
  if (!mixedPreservedZeroTokenGapPieces.empty()) {
    bool insertedSeparator = false;
    for (const TUPreservedGapPiece &piece : mixedPreservedZeroTokenGapPieces) {
      std::string pieceText = preservedTUGapPieceText(piece);
      if (pieceText.empty())
        continue;

      if (piece.kind == TUPreservedGapPiece::Kind::BalancedPragmaStateIsland &&
          balancedDiagnosticPragmaStateIslandIsCarriedByReplacement(
              StringRef(pieceText), StringRef(replacement), lexLang_)) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU/include closure: balanced pragma-state island id={0} "
            "source=[{1},{2}) already carried by B replacement",
            piece.id, piece.begin, piece.end);
        continue;
      }

      if (!insertedSeparator && !replacement.empty() &&
          replacement.back() != '\n') {
        replacement.push_back('\n');
        insertedSeparator = true;
      }
      replacement += pieceText;
      if (replacement.empty() || replacement.back() != '\n')
        replacement.push_back('\n');
    }
  }

  if (!mixedPreservedSourceLineDirectiveGapPieces.empty()) {
    for (const auto &piece : mixedPreservedSourceLineDirectiveGapPieces) {
      if (piece.first > piece.second || piece.second > tuBytes.size())
        continue;

      StringRef pieceText = tuBytes.slice(piece.first, piece.second);

      // The preserved source gap is sliced from the original file bytes.  When
      // the preceding replacement material already ends at the beginning of a
      // fresh physical line, a leading line break in the source slice is the
      // same separator, not an additional intentional blank line.  Drop only
      // that duplicated separator; any further blank lines remain preserved.
      if (!replacement.empty() && replacement.back() == '\n') {
        if (pieceText.starts_with("\r\n"))
          pieceText = pieceText.drop_front(2);
        else if (pieceText.starts_with("\n") || pieceText.starts_with("\r"))
          pieceText = pieceText.drop_front(1);
      }

      if (!replacement.empty() && replacement.back() != '\n' &&
          !pieceText.starts_with("\n") && !pieceText.starts_with("\r"))
        replacement.push_back('\n');

      replacement.append(pieceText.begin(), pieceText.end());
      if (replacement.empty() || replacement.back() != '\n')
        replacement.push_back('\n');
    }
  }

  // The closure begins at the proved source interval.  For a pure include
  // closure that is the include-directive run; for a mixed TU/include closure
  // it also includes the directly consumed TU tokens adjacent to that run.  We
  // may later extend the right edge over immediately following untouched TU
  // line(s) so any required physical-line compensation lands after copied
  // suffix lines such as `};` instead of inside the realized replacement
  // itself.
  uint64_t closureSourceEnd = sourceEnd;
  std::string padded = refoldPadAtBoundaries(
      tuBytes, static_cast<size_t>(sourceBegin),
      static_cast<size_t>(closureSourceEnd), std::move(replacement),
      /*allowLeft=*/true, /*allowRight=*/true, lexLang_);

  const bool emitsSourceLineDirectiveResume = lineDirs_.Enabled() &&
                                              mixedSourceLineDirectiveResume &&
                                              sourceEnd < tuBytes.size();
  if (emitsSourceLineDirectiveResume) {
    // The consumed source envelope contained a source-spelled line-control
    // directive.  Re-emit only its net state, adjusted through the consumed
    // envelope, so the untouched suffix observes the same presumed file/line as
    // it did in the original source. Replacement hunks normally carry edited B
    // material before the resync; delete-only hunks have no such payload, so
    // they may need a leading physical newline to make the resync a standalone
    // preprocessing directive rather than gluing `#line` onto the preceding
    // source line.
    if (padded.empty()) {
      if (!lineDirectiveStartsAtPrefix(sourceBegin))
        padded.push_back('\n');
    } else if (padded.back() != '\n') {
      padded.push_back('\n');
    }

    padded +=
        formatSourceLineDirectiveGapResume(*mixedSourceLineDirectiveResume);
    REFOLD_LOG_TRACE(
        "fallback",
        "TU/include closure emitted source #line resume line={0} file={1}",
        mixedSourceLineDirectiveResume->lineAtResume,
        mixedSourceLineDirectiveResume->fileSpelling);
  }

  // This closure class replaces only a contiguous run of top-level include
  // directives plus admitted inert trivia between them. Preserve the original
  // physical line count of the replaced source so the untouched suffix can
  // resume on its original logical line without needing a #line directive.
  // When there is a deficit, first try to sink it past immediately following
  // untouched TU
  // line(s) by absorbing those exact source bytes into the same TU edit.
  const size_t origNl =
      stringutils::countNewlines(tuBytes, sourceBegin, closureSourceEnd);
  const size_t initialReplNl = stringutils::countNewlines(padded);
  if (!emitsSourceLineDirectiveResume && origNl > initialReplNl) {
    size_t remainingPadNl = origNl - initialReplNl;

    // Return true when the source interval overlaps a spelled include directive
    // or macro invocation in this TU. Widened include-closure edits must not
    // absorb such artifacts implicitly.
    auto intervalOverlapsSpelledArtifact = [&](uint64_t begin,
                                               uint64_t end) -> bool {
      for (const auto &inc : model_.GetIncludes()) {
        if (!paths_.PathsEqual(inc.sitePath, tuPath))
          continue;
        if (inc.siteB < end && begin < inc.siteE)
          return true;
      }
      for (const auto &m : model_.GetMacroInvocations()) {
        if (m.invFile && !m.invFile->empty() &&
            !paths_.PathsEqual(*m.invFile, tuPath))
          continue;
        if (!m.invB || !m.invE)
          continue;
        if (*m.invB < end && begin < *m.invE)
          return true;
      }
      return false;
    };

    if (remainingPadNl > 0 && closureSourceEnd < tuBytes.size()) {
      const uint64_t lineBegin = closureSourceEnd;
      uint64_t lineEnd = lineBegin;
      while (lineEnd < tuBytes.size() && tuBytes[lineEnd] != '\n')
        ++lineEnd;
      if (lineEnd < tuBytes.size())
        ++lineEnd;

      // Sink the unavoidable physical-line compensation past at most one
      // immediately following substantive TU line. This is enough to move the
      // blank-line surrogate past a copied suffix line such as `};`, while
      // remaining conservative: copied-line absorption is newline-deficit
      // invariant, so absorbing additional lines would only push the eventual
      // compensation point farther down the file without improving correctness.
      if (lineEnd > lineBegin &&
          !intervalOverlapsSpelledArtifact(lineBegin, lineEnd)) {
        StringRef line = tuBytes.slice(lineBegin, lineEnd);
        StringRef trimmed = line.trim();
        if (!trimmed.empty() && trimmed.front() != '#') {
          StringRef copiedLine = line;

          // The sink may start in the middle of a physical source line, for
          // example after the TU/include closure consumed the leading token of
          // `int between = 0;`.  In that case `padded` already contains the
          // B-side spelling and any B-side separator after that token, while
          // `line` begins with the original source separator before the next
          // token.  Keeping both separators manufactures visible whitespace
          // such as `int  between`.  Collapse exactly one copied horizontal
          // source byte only when the sink is a same-line continuation and the
          // replacement already ends in horizontal whitespace.  Remaining
          // source whitespace, if any, is preserved byte-for-byte.
          if (lineBegin > 0 && tuBytes[lineBegin - 1] != '\n' &&
              tuBytes[lineBegin - 1] != '\r' && !padded.empty() &&
              stringutils::isNonNewlineWs(padded.back()) &&
              !copiedLine.empty() &&
              stringutils::isNonNewlineWs(copiedLine.front())) {
            copiedLine = copiedLine.drop_front();
          }

          padded.append(copiedLine.begin(), copiedLine.end());
          closureSourceEnd = lineEnd;

          // Re-emitting one untouched trailing line verbatim moves the eventual
          // physical-line compensation point to just after that copied line,
          // but it does not reduce the outstanding newline deficit. The
          // original source span grows by the same newline count as the
          // replacement text, so the difference between them is invariant under
          // this exact copied-line absorption.
          REFOLD_LOG_TRACE(
              "fallback",
              "TU include-closure newline sink: absorbed source=[{0},{1}) "
              "line='{2}' copiedLine='{3}' remainingPad={4} "
              "(single-line invariant-preserving sink)",
              lineBegin, lineEnd, stringutils::showWsWithClip(line, 120),
              stringutils::showWsWithClip(copiedLine, 120), remainingPadNl);
        }
      }
    }

    if (remainingPadNl > 0) {
      if (lineDirs_.Enabled()) {
        // Keep line-preservation semantic rather than physical in normal mode:
        // suppress synthetic blank padding here and let
        // hooks_.applyResyncOrPend() observe the unchanged newline deficit over
        // the final edit span. If a resync is needed, it will be emitted as a
        // #line directive after any copied suffix line absorbed above.
        REFOLD_LOG_TRACE(
            "fallback",
            "TU include-closure physical newline pad deferred to #line "
            "resync: source=[{0},{1}) suppressedNl={2}",
            sourceBegin, closureSourceEnd, remainingPadNl);
      } else {
        // In --no-lines mode, clang-refold intentionally does not preserve
        // source-location semantics. The verification path already masks
        // location-sensitive predefined macro differences for this mode, so
        // newline-deficit padding would only manufacture visible blank lines in
        // the refolded source. Boundary padding above is still responsible for
        // lexical separation; this padding is solely for physical line count.
        REFOLD_LOG_TRACE(
            "fallback",
            "TU include-closure physical newline pad suppressed by "
            "--no-lines: source=[{0},{1}) suppressedNl={2}",
            sourceBegin, closureSourceEnd, remainingPadNl);
      }
      remainingPadNl = 0;
    }

    if (remainingPadNl > 0)
      padded.append(remainingPadNl, '\n');

    REFOLD_LOG_TRACE(
        "fallback",
        "TU include-closure newline pad: source=[{0},{1}) origNl={2} "
        "replNl={3} physicalPadNl={4} finalSourceEnd={5} result='{6}'",
        sourceBegin, sourceEnd, origNl, initialReplNl, remainingPadNl,
        closureSourceEnd, stringutils::showWsWithClip(padded, 120));
  }

  // If this closure deletes the include instance that originally entered a
  // `#pragma once` header, a later same-header include that was token-empty in
  // A can become live in the refolded source.  Leaving that later directive in
  // place would replay declarations/tokens from the header that are not present
  // in B.  This is a source-order effect of `#pragma once`, not an ordinary
  // token-cover issue: the skipped include has no A cover, so it will not be
  // found by the hunk overlap logic above.
  //
  // The deterministic repair for this TU include-closure class is to extend the
  // same TU byte replacement over any immediately-reachable skipped same-header
  // include directives.  We only cross trivia between the current closure end
  // and the skipped directive.  If substantive source lies between them, this
  // proof class cannot decide whether that source observes the moved header
  // state, so it rejects instead of emitting a refolding that would reactivate
  // the include.
  // A consumed include only creates the reactivation hazard when the
  // physical header is known to carry include-local once state.  We use the
  // recorded pragma table instead of guessing from include emptiness: an empty
  // later include can have other causes, but a consumable `#pragma once` in the
  // resolved header gives the exact source-order state transition we must
  // preserve.
  auto pathHasPragmaOnce = [&](StringRef resolvedPath) -> bool {
    for (const auto &pragma : model_.GetPragmas())
      if (paths_.PathsEqual(pragma.sitePath, resolvedPath) &&
          pragmaEstablishesIncludeOnceState(pragma))
        return true;
    return false;
  };

  // Interpret "consumed" in the final widened source interval, not in the
  // original hunk.  The closure may have already been padded/widened above, and
  // the reactivation proof must reason about the exact bytes that will be
  // replaced.
  auto includeIntervalIsConsumed = [&](const RefoldModel::IncludeItem &inc) {
    return sourceBegin <= inc.siteB && inc.siteE <= closureSourceEnd;
  };

  // Collect each resolved `#pragma once` header whose entering include is
  // removed by this closure.  Later skipped includes are matched by resolved
  // path, so repeated textual spellings such as "x.h" and "./x.h" collapse
  // to the same include-once state when the producer resolved them that way.
  SmallVector<StringRef, 4> consumedPragmaOncePaths;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (!inc->resolvedPath || !includeIntervalIsConsumed(*inc))
      continue;
    if (!pathHasPragmaOnce(*inc->resolvedPath))
      continue;
    if (!llvm::any_of(consumedPragmaOncePaths, [&](StringRef path) {
          return paths_.PathsEqual(path, *inc->resolvedPath);
        }))
      consumedPragmaOncePaths.push_back(*inc->resolvedPath);
  }

  auto includeMatchesConsumedPragmaOncePath =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    if (!inc.resolvedPath)
      return false;
    return llvm::any_of(consumedPragmaOncePaths, [&](StringRef path) {
      return paths_.PathsEqual(path, *inc.resolvedPath);
    });
  };

  // A skipped include is only dangerous if removing the closure would make it
  // the first preserved include of that resolved header.  If some earlier
  // same-header include remains before the candidate, that prior include still
  // establishes the `#pragma once` state and the candidate remains skipped.
  auto hasPreservedPriorSameHeaderInclude =
      [&](const RefoldModel::IncludeItem &candidate) -> bool {
    if (!candidate.resolvedPath)
      return false;
    for (const auto &prior : model_.GetIncludes()) {
      if (prior.id == candidate.id || prior.parent ||
          !paths_.PathsEqual(prior.sitePath, tuPath) || !prior.resolvedPath)
        continue;
      if (!paths_.PathsEqual(*prior.resolvedPath, *candidate.resolvedPath))
        continue;
      if (prior.siteE > candidate.siteB)
        continue;
      if (!includeIntervalIsConsumed(prior))
        return true;
    }
    return false;
  };

  auto extendOverReactivatedPragmaOnceIncludes = [&]() -> bool {
    if (consumedPragmaOncePaths.empty())
      return true;

    while (true) {
      const RefoldModel::IncludeItem *next = nullptr;

      // Pick the earliest skipped same-header include that would become live
      // after the current closure.  The loop repeats because consuming that
      // directive can expose another immediately-following skipped include of
      // the same header.  Ties are broken by item id to keep the proof
      // deterministic for equal byte offsets.
      for (const auto &inc : model_.GetIncludes()) {
        if (inc.parent || !paths_.PathsEqual(inc.sitePath, tuPath))
          continue;
        if (inc.siteB < closureSourceEnd)
          continue;
        // Only originally-skipped includes have no A-token cover.  Includes
        // with a cover already participated in normal token/hunk closure logic.
        if (inc.cover.IsValid())
          continue;
        if (!includeMatchesConsumedPragmaOncePath(inc))
          continue;
        if (hasPreservedPriorSameHeaderInclude(inc))
          continue;
        // Do not silently erase a skipped include if the map recorded effects
        // on that include instance.  This repair is only for the pure
        // `#pragma once` reactivation case where the directive was token-empty
        // because a prior include had already entered the header.
        if (includeHasRecordedSideEffects(inc))
          continue;
        if (!next || inc.siteB < next->siteB ||
            (inc.siteB == next->siteB && inc.id < next->id))
          next = &inc;
      }

      if (!next)
        return true;

      if (next->siteB > tuBytes.size() || next->siteE > tuBytes.size() ||
          next->siteB > next->siteE) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU include-closure rejected: skipped pragma-once include "
            "inc#{0} has invalid site=[{1},{2})",
            next->id, next->siteB, next->siteE);
        return false;
      }

      // Crossing only trivia is what makes the widened edit local: no
      // preserved source between the deleted entering include and the skipped
      // include can observe the header macro state at the old position.
      StringRef gap = tuBytes.slice(closureSourceEnd, next->siteB);
      if (!gapIsIndexedLexerTrivia(closureSourceEnd, next->siteB)) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU include-closure rejected: consuming earlier #pragma once "
            "include would reactivate skipped include inc#{0}, but gap "
            "source=[{1},{2}) is not trivia: '{3}'",
            next->id, closureSourceEnd, next->siteB,
            stringutils::showWsWithClip(gap, 120));
        return false;
      }

      if (sourceTouchesStagedEdit(sourceBegin, next->siteE)) {
        REFOLD_LOG_TRACE(
            "fallback",
            "TU include-closure rejected: extending over reactivated "
            "#pragma once include inc#{0} to sourceEnd={1} would overlap "
            "an already-staged source edit",
            next->id, next->siteE);
        return false;
      }

      REFOLD_LOG_TRACE(
          "fallback",
          "TU include-closure extending over skipped #pragma once include "
          "inc#{0} path='{1}' source=[{2},{3}) after consuming earlier "
          "include instance",
          next->id, next->resolvedPath ? *next->resolvedPath : next->target,
          next->siteB, next->siteE);
      closureSourceEnd = next->siteE;
    }
  };

  if (!extendOverReactivatedPragmaOnceIncludes())
    return std::nullopt;

  REFOLD_LOG_DEBUG(
      "fallback",
      "TU include-closure accepted: includes=[{0},{1}] source=[{2},{3}) "
      "A=[{4},{5}) B=[{6},{7}) evidence={8} raw='{9}' padded='{10}'",
      first->id, last->id, sourceBegin, closureSourceEnd, materialBeginA,
      materialEndA, bEnvelope->first, bEnvelope->second, evidenceKind,
      stringutils::showWsWithClip(rawReplacement, 120),
      stringutils::showWsWithClip(padded, 120));

  ResyncOutcome ro =
      emitsSourceLineDirectiveResume
          ? ResyncOutcome(padded, std::nullopt)
          : hooks_.applyResyncOrPend(tuBytes, sourceBegin, closureSourceEnd,
                                     padded, tuPath, std::nullopt);
  TextEdit edit{sourceBegin,
                closureSourceEnd,
                std::move(ro.text),
                std::move(ro.pending),
                std::nullopt,
                {},
                {},
                {}};
  // A source-#line resume emits no prune candidates; only the ordinary resync
  // path forwards them.
  if (!emitsSourceLineDirectiveResume)
    edit.lineControlPruneCandidates = std::move(ro.lineControlPruneCandidates);
  hooks_.certifyTextEditMaterializedBTokenRange(edit, bEnvelope->first,
                                                bEnvelope->second);
  if (!hooks_.authorizeTUIncludeClosure ||
      !hooks_.authorizeTUIncludeClosure(edit, tuPath, tuBytes, sourceBegin,
                                        closureSourceEnd))
    return std::nullopt;

  // Emit a first-class TU include-closure carrier instead of hiding this hybrid
  // proof behind the ordinary conservative TU byte-span class.  The local
  // checks above are the closure witness: exact/widened include cover,
  // preservable source gaps, canonical-or-consensus B envelope, line-control
  // repair, and pragma-once/include-guard reactivation safety all had to be
  // discharged before this edit was created.
  AcceptedResultCandidate acceptedClosure =
      proofLattice_.AcceptedCandidateBuilder()
          .BuildAcceptedSpecializedTUTextEditCandidate(
              AcceptedPathKind::TUIncludeClosureEdit, sourceBegin,
              closureSourceEnd, StringRef(rawReplacement));
  theoremAuditService_.AuditExpansionFallbackAcceptedCandidate(
      fallbackBranch, acceptedClosure,
      "BuildTUIncludeClosureEditForUnresolvedHunk/accepted");
  hooks_.attachAcceptedResultCarrier(edit, acceptedClosure);
  return edit;
}

std::string RefoldExpansionFallbackPlanner::ResolvePostStructuralFallback() {
  if (!terminalSink_.HasRequest())
    return std::string();

  const ExpansionFallbackBranchClassification terminalBranch =
      ClassifyExpansionFallbackBranch(
          ExpansionFallbackBranchKind::PostStructuralTerminalOutOfDomain);
  theoremAuditService_.AuditExpansionFallbackBranchClassification(
      terminalBranch, "ResolvePostStructuralFallback");

  // Reaching this point means every structural proof lattice candidate has
  // failed closed or has been classified out-of-domain.  A macro whole-cover
  // case is in-domain only if OwnerRealizationProof or MixedOwnerTilingProof
  // accepted it before fallback was requested.  Do not run a second
  // post-terminal owner search here; normalize directly onto the declared
  // TerminalOutOfDomain carrier.
  TerminalFallbackWitness terminalWitness =
      proofLattice_.BuildTerminalFallbackWitness();
  bool terminalAuditOk = true;
  for (size_t i = 0; i < terminalWitness.proofFailures.size(); ++i) {
    const llvm::StringRef role =
        i == 0 ? llvm::StringRef("primary") : llvm::StringRef("secondary");
    terminalAuditOk &= theoremAuditService_.AuditTerminalFallbackProofFailure(
        terminalWitness.proofFailures[i], role);
  }
  if (terminalWitness.proofFailures.empty()) {
    terminalAuditOk = false;
    theoremAuditService_.RecordTerminalFallbackProofFailureListMissing(
        "terminal fallback had no structured failed-obligation list");
  }
  if (!terminalAuditOk) {
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
            TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
            TerminalFallbackFailureContext::ForStateComponent(
                "terminalFallbackAudit")),
        "terminal/audit",
        "terminal fallback proof-failure audit rejected the raw-B carrier");
    terminalWitness = proofLattice_.BuildTerminalFallbackWitness();
  }

  theoremAuditService_.RecordTerminalFallbackTheoremAudit();

  REFOLD_LOG_DEBUG(
      "fallback",
      "terminal fallback: emitting fully expanded edited preprocessed "
      "stream (B). reasons={0} proofFailures={1}",
      terminalSink_.Requests().size(), terminalWitness.proofFailures.size());
  const AcceptedResultCandidate terminalCandidate =
      proofLattice_.AcceptedCandidateBuilder().BuildAcceptedTerminalCandidate(
          terminalWitness);
  theoremAuditService_.AuditExpansionFallbackAcceptedCandidate(
      terminalBranch, terminalCandidate,
      "ResolvePostStructuralFallback/terminal");
  for (const TerminalFallbackRequest &request : terminalSink_.Requests())
    REFOLD_LOG_DEBUG("fallback", "  {0}", request);

  // The raw-B terminal carrier is the fully expanded surface, so every include
  // and every top-level macro root remains expanded in the final result.
  hooks_.resetAttemptStats();
  lastStats_.expandedIncludes = lastStats_.totalIncludes;
  lastStats_.expandedMacros = lastStats_.totalMacros;
  if (materializedEditMappings_) {
    materializedEditMappings_->clear();
    materializedEditMappings_->push_back(
        MaterializedEditMapping{0, static_cast<uint64_t>(bSource_.size()), 0,
                                static_cast<uint64_t>(bSource_.size())});
  }
  return bSource_.str();
}
} // namespace refold
} // namespace clang
