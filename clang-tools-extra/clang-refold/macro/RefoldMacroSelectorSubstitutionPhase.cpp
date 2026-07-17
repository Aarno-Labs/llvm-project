//===--- RefoldMacroSelectorSubstitutionPhase.cpp ---------------*- C++ -*-===//
//
// Callee-substitution fallback phase.
//
// This translation unit owns the fallback candidate paths that derive an
// invocation-preserving callee rewrite from producer function-like macro
// definitions and validate the resulting candidate against the current
// whole-cover planning context.  The direct path rewrites a literal callsite
// callee name, while the paste-derived path rewrites a producer-proven selector
// slice that formed a descendant callee.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroSelectorSubstitutionPhase.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

using MacroTokenSpellings = SmallVector<std::string, 16>;
using MacroArgumentTokenSpellings = SmallVector<MacroTokenSpellings, 4>;

/// Producer-recorded function-like macro definition selected from the active
/// macro-state tape.  The directive pointer is model-owned and remains valid for
/// the phase lifetime.
struct ProducerFunctionMacroDefinition {
  const RefoldModel::MacroDirective *directive = nullptr;
  StringRef name;
};

/// Trace counters for the direct-callee proof search.  They are deliberately
/// diagnostic-only: the semantic path still accepts or rejects from the same
/// proof predicates, while this ledger names the first predicate that pruned
/// each alternate definition.
struct DirectCalleeSubstitutionTraceStats {
  unsigned definitionsScanned = 0;
  unsigned nonFunctionDefinitions = 0;
  unsigned originalName = 0;
  unsigned inactiveDefinitions = 0;
  unsigned arityMismatches = 0;
  unsigned replayFailures = 0;
  unsigned bReplayMismatches = 0;
  unsigned noopReplacements = 0;
  unsigned replacementArgRangeFailures = 0;
  unsigned shapeFailures = 0;
  unsigned accepted = 0;
};

/// Stable spelling for optional token/source offsets in trace diagnostics.
static std::string formatOptionalU64(std::optional<uint64_t> value) {
  if (!value)
    return "<none>";
  return std::to_string(*value);
}

/// Emit one direct-callee rejection record.  Keeping all rejection records on
/// the same category makes the failing predicate grep-able in lit artifacts.
static void traceDirectCalleeReject(const RefoldModel::MacroInvocation &m,
                                    StringRef reason) {
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "reject inv id={0} name={1} reason={2}", m.id, m.name,
                   reason);
}

/// Emit one direct-callee rejection record with a compact structured detail
/// payload.  Details are strings so callers can format only the values they
/// know at the rejection site without adding semantic dependencies.
static void traceDirectCalleeReject(const RefoldModel::MacroInvocation &m,
                                    StringRef reason, StringRef detail) {
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "reject inv id={0} name={1} reason={2} detail={3}", m.id,
                   m.name, reason, detail);
}

/// Emit one alternate-definition rejection record from the candidate loop.
static void traceDirectCalleeCandidateReject(
    const RefoldModel::MacroInvocation &m,
    const RefoldModel::MacroDirective &directive, StringRef reason) {
  REFOLD_LOG_TRACE(
      "macro/direct-callee",
      "candidate-reject inv id={0} name={1} directive id={2} candidate={3} "
      "reason={4}",
      m.id, m.name, directive.id, directive.name, reason);
}

/// Emit the diagnostic-only alternate-definition search summary.
static void traceDirectCalleeSearchSummary(
    const RefoldModel::MacroInvocation &m,
    const DirectCalleeSubstitutionTraceStats &stats,
    unsigned acceptedReplacementCount) {
  REFOLD_LOG_TRACE(
      "macro/direct-callee",
      "summary inv id={0} name={1} scanned={2} nonFunction={3} originalName={4} "
      "inactive={5} arityMismatch={6} replayFailure={7} bReplayMismatch={8} "
      "noop={9} argRangeFailure={10} shapeFailure={11} accepted={12} "
      "replacementClasses={13}",
      m.id, m.name, stats.definitionsScanned, stats.nonFunctionDefinitions,
      stats.originalName, stats.inactiveDefinitions, stats.arityMismatches,
      stats.replayFailures, stats.bReplayMismatches, stats.noopReplacements,
      stats.replacementArgRangeFailures, stats.shapeFailures, stats.accepted,
      acceptedReplacementCount);
}

/// Return a replayable function-like #define directive, or nullopt for entries
/// that are not function-like macro definitions.  The consumer intentionally
/// uses only producer-parsed macro-state fields here; it does not reparse raw
/// directive text.
static std::optional<ProducerFunctionMacroDefinition>
producerFunctionDefinitionFromDirective(
    const RefoldModel::MacroDirective &directive) {
  if (directive.subkind != "#define" || directive.name.empty() ||
      !directive.functionLike)
    return std::nullopt;

  ProducerFunctionMacroDefinition definition;
  definition.directive = &directive;
  definition.name = directive.name;
  return definition;
}

/// Resolve the active function-like macro definition for \p name immediately
/// before a producer item id.  Undefs close the definition state, so an inactive
/// name cannot be resurrected as an alternate callee candidate.
static std::optional<ProducerFunctionMacroDefinition>
activeFunctionDefinitionBefore(const RefoldModel &model, uint64_t beforeItemId,
                               StringRef name) {
  const RefoldModel::MacroDirective *active = nullptr;
  for (const RefoldModel::MacroDirective &directive :
       model.GetMacroDirectives()) {
    if (directive.id >= beforeItemId)
      continue;
    if (directive.subkind != "#define" && directive.subkind != "#undef")
      continue;
    if (directive.name != name)
      continue;
    if (!active || directive.id > active->id)
      active = &directive;
  }
  if (!active || active->subkind != "#define")
    return std::nullopt;
  return producerFunctionDefinitionFromDirective(*active);
}

/// Replay a producer-recorded replacement-list tape under recovered actual
/// argument token spellings.  `#` and `##` are deliberately out of domain for
/// this callee-substitution proof because their spelling semantics require the
/// dedicated stringify/paste proof families.
static std::optional<MacroTokenSpellings> replayProducerFunctionMacroTokens(
    const ProducerFunctionMacroDefinition &definition,
    ArrayRef<MacroTokenSpellings> actualArgs) {
  const RefoldModel::MacroDirective &directive = *definition.directive;
  if (directive.defParams.size() != actualArgs.size())
    return std::nullopt;

  MacroTokenSpellings replayed;
  for (const RefoldModel::MacroReplacementToken &token :
       directive.replacementTokens) {
    if (token.spelling == "#" || token.spelling == "##")
      return std::nullopt;
    switch (token.kind) {
    case RefoldModel::MacroReplacementTokenKind::Literal:
      replayed.push_back(token.spelling.str());
      break;
    case RefoldModel::MacroReplacementTokenKind::ParamRef:
      if (!token.paramIndex || *token.paramIndex >= actualArgs.size())
        return std::nullopt;
      replayed.append(actualArgs[*token.paramIndex].begin(),
                      actualArgs[*token.paramIndex].end());
      break;
    }
  }
  return replayed;
}

/// Recover each source actual's A-side token spelling from producer argument
/// spans.  Every formal must have at least one recorded occurrence, and repeated
/// occurrences of the same formal must agree exactly.  This stronger-than-needed
/// requirement keeps direct callee substitution fail-closed when a future
/// alternate definition might use a formal that the original body did not expose
/// in the PP cover.
static std::optional<MacroArgumentTokenSpellings>
recoverActualArgumentSpellingsFromArgSpans(
    const RefoldModel::MacroInvocation &invocation, ArrayRef<PPTok> aToks) {
  if (invocation.defParams.empty())
    return MacroArgumentTokenSpellings{};

  MacroArgumentTokenSpellings actualArgs;
  actualArgs.resize(invocation.defParams.size());
  SmallVector<char, 8> seen;
  seen.resize(invocation.defParams.size(), 0);

  for (const RefoldModel::PPArgSpan &span : invocation.argSpans) {
    if (span.argIdx >= invocation.defParams.size() || span.end < span.begin ||
        span.end > aToks.size())
      return std::nullopt;

    MacroTokenSpellings spelling;
    for (uint64_t tok = span.begin; tok < span.end; ++tok)
      spelling.push_back(aToks[static_cast<size_t>(tok)].spelling);

    if (seen[span.argIdx] &&
        !tokenSpellingVectorsEqual(actualArgs[span.argIdx], spelling))
      return std::nullopt;

    actualArgs[span.argIdx] = std::move(spelling);
    seen[span.argIdx] = 1;
  }

  for (char formalSeen : seen)
    if (!formalSeen)
      return std::nullopt;
  return actualArgs;
}

/// B-side spelling and source text for one producer-recovered actual.
/// Direct-callee substitution may need both views: token spellings drive the
/// definition replay proof against B, while source text is written back into
/// the invocation argument slot when the same root cover also carries an
/// args-only edit.
struct BActualArgumentReplaySurface {
  MacroTokenSpellings tokenSpellings;
  std::string sourceText;
};

using BActualArgumentReplaySurfaces =
    SmallVector<BActualArgumentReplaySurface, 4>;

/// Recover B-side actual spellings by projecting each producer arg span through
/// the A->B byte mapper.  Repeated formal occurrences must agree in token
/// spelling; otherwise a single rewritten invocation argument would not be a
/// deterministic explanation of the edited B expansion.
static std::optional<BActualArgumentReplaySurfaces>
recoverBActualArgumentReplaySurfaces(
    const RefoldModel::MacroInvocation &invocation,
    const RefoldSourceMapper &sourceMapper, ArrayRef<PPTok> bToks) {
  if (invocation.defParams.empty())
    return BActualArgumentReplaySurfaces{};

  BActualArgumentReplaySurfaces actuals;
  actuals.resize(invocation.defParams.size());
  SmallVector<char, 8> seen;
  seen.resize(invocation.defParams.size(), 0);

  for (const RefoldModel::PPArgSpan &span : invocation.argSpans) {
    if (span.argIdx >= invocation.defParams.size())
      return std::nullopt;

    std::optional<std::pair<size_t, size_t>> bEnv =
        sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(span);
    if (!bEnv || bEnv->second < bEnv->first || bEnv->second > bToks.size())
      return std::nullopt;

    BActualArgumentReplaySurface surface;
    for (size_t tok = bEnv->first; tok < bEnv->second; ++tok)
      surface.tokenSpellings.push_back(bToks[tok].spelling);
    surface.sourceText = sourceMapper.SliceBSource(bEnv->first, bEnv->second)
                             .trim()
                             .str();

    if (seen[span.argIdx] &&
        !tokenSpellingVectorsEqual(actuals[span.argIdx].tokenSpellings,
                                   surface.tokenSpellings))
      return std::nullopt;

    actuals[span.argIdx] = std::move(surface);
    seen[span.argIdx] = 1;
  }

  for (char formalSeen : seen)
    if (!formalSeen)
      return std::nullopt;
  return actuals;
}

/// Return just the token-spelling view needed by replacement-list replay.
static MacroArgumentTokenSpellings tokenSpellingsFromBActualSurfaces(
    ArrayRef<BActualArgumentReplaySurface> actuals) {
  MacroArgumentTokenSpellings spellings;
  spellings.reserve(actuals.size());
  for (const BActualArgumentReplaySurface &actual : actuals)
    spellings.push_back(actual.tokenSpellings);
  return spellings;
}

/// Accepted direct-callee substitution witness.  Keeping this type at file
/// scope lets the uniqueness ordering stay named and reusable instead of being
/// hidden in an in-function comparator.
struct DirectCalleeSubstitutionCandidate {
  uint64_t candidateDirectiveId = 0;
  uint64_t bTokStart = 0;
  uint64_t bTokEnd = 0;
  uint64_t newCalleeSize = 0;
  std::string replacement;
};

/// Deterministic ordering for direct-callee substitution witnesses.  The proof
/// later accepts only one replacement spelling, but sorting first makes both
/// tie handling and diagnostics stable.
static bool directCalleeSubstitutionCandidateLess(
    const DirectCalleeSubstitutionCandidate &lhs,
    const DirectCalleeSubstitutionCandidate &rhs) {
  if (lhs.replacement != rhs.replacement)
    return lhs.replacement < rhs.replacement;
  if (lhs.candidateDirectiveId != rhs.candidateDirectiveId)
    return lhs.candidateDirectiveId < rhs.candidateDirectiveId;
  if (lhs.bTokStart != rhs.bTokStart)
    return lhs.bTokStart < rhs.bTokStart;
  return lhs.bTokEnd < rhs.bTokEnd;
}

/// Return the byte range of the leading macro-name token inside a callsite text
/// that was already admitted by InvocationSpanMatchesCallsitePrefix().  The
/// range is relative to the replacement text carried by MacroPatch.
static std::optional<std::pair<uint64_t, uint64_t>>
findDirectCalleeNameByteRange(StringRef invSpanText,
                              const RefoldModel::MacroInvocation &invocation) {
  if (invSpanText.empty() || invocation.name.empty())
    return std::nullopt;

  const size_t n = invSpanText.size();
  size_t identBegin = 0;
  while (identBegin < n && stringutils::isWs(invSpanText[identBegin]))
    ++identBegin;
  if (identBegin >= n || !stringutils::isIdentStart(invSpanText[identBegin]))
    return std::nullopt;

  size_t identEnd = identBegin + 1;
  while (identEnd < n && stringutils::isIdentPart(invSpanText[identEnd]))
    ++identEnd;

  if (invSpanText.slice(identBegin, identEnd) != invocation.name)
    return std::nullopt;

  return std::make_pair(static_cast<uint64_t>(identBegin),
                        static_cast<uint64_t>(identEnd));
}

/// Build the source replacement for direct-callee substitution.  The rewrite is
/// deliberately limited to the leading callee token plus producer-identified
/// argument content ranges.  That is the shape needed for mixed edits such as
/// `ADD(5, 20)` -> `SUB(15, 24)`: exact definition replay proves the changed
/// macro-body token, and the argument ranges carry the ordinary args-only
/// edits without allowing any unmodelled fixed syntax to move.
static std::optional<std::string> buildDirectCalleeReplacementText(
    StringRef baseText,
    std::pair<uint64_t, uint64_t> calleeByteRange,
    StringRef newCalleeSpelling,
    ArrayRef<std::pair<size_t, size_t>> baseArgRanges,
    ArrayRef<BActualArgumentReplaySurface> bActuals) {
  const uint64_t calleeBegin = calleeByteRange.first;
  const uint64_t calleeEnd = calleeByteRange.second;
  if (calleeBegin >= calleeEnd || calleeEnd > baseText.size() ||
      newCalleeSpelling.empty())
    return std::nullopt;
  if (baseArgRanges.size() != bActuals.size())
    return std::nullopt;

  if (!stringutils::isIdentStart(newCalleeSpelling.front()))
    return std::nullopt;
  for (char c : newCalleeSpelling.drop_front())
    if (!stringutils::isIdentPart(c))
      return std::nullopt;

  std::string replacement = baseText.str();

  // Argument slots are after the callee token in a function-like invocation.
  // Apply them right-to-left so earlier replacement offsets remain stable.
  for (size_t idx = baseArgRanges.size(); idx > 0; --idx) {
    const auto &baseRange = baseArgRanges[idx - 1];
    if (baseRange.first > baseRange.second ||
        baseRange.second > baseText.size() || baseRange.first < calleeEnd)
      return std::nullopt;

    replacement.replace(static_cast<size_t>(baseRange.first),
                        static_cast<size_t>(baseRange.second - baseRange.first),
                        bActuals[idx - 1].sourceText);
  }

  replacement.replace(static_cast<size_t>(calleeBegin),
                      static_cast<size_t>(calleeEnd - calleeBegin),
                      newCalleeSpelling.str());
  return replacement;
}

} // namespace

RefoldMacroSelectorSubstitutionPhase::RefoldMacroSelectorSubstitutionPhase(
    Dependencies deps)
    : deps_(std::move(deps)) {}

std::optional<MacroPatch>
RefoldMacroSelectorSubstitutionPhase::TryDirectCalleeSubstitution(
    const RefoldMacroWholeCoverPlanningContext &planningCtx,
    StringRef invSpanText) const {
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &hEff = planningCtx.hEff;

  REFOLD_LOG_TRACE(
      "macro/direct-callee",
      "enter inv id={0} name={1} subkind={2} calleeOrigin={3} "
      "cover=[{4},{5}) hEffA=[{6},{7}) hEffB=[{8},{9}) bodySpans={10} "
      "argSpans={11} invBytes=[{12},{13}) invText='{14}'",
      m.id, m.name, m.subkind, static_cast<unsigned>(m.calleeOrigin.kind),
      m.cover.begin, m.cover.end, hEff.aStart, hEff.aEnd, hEff.bStart,
      hEff.bEnd, m.bodySpans.size(), m.argSpans.size(),
      formatOptionalU64(m.invB), formatOptionalU64(m.invE),
      stringutils::showWsWithClip(invSpanText, 220));

  if (m.subkind != "func") {
    traceDirectCalleeReject(m, "not-function-like", m.subkind);
    return std::nullopt;
  }
  if (m.calleeOrigin.kind != MacroCalleeOriginKind::LiteralMacroName) {
    traceDirectCalleeReject(
        m, "callee-origin-not-literal",
        llvm::formatv("kind={0}", static_cast<unsigned>(m.calleeOrigin.kind))
            .str());
    return std::nullopt;
  }
  if (!m.cover.IsValid()) {
    traceDirectCalleeReject(m, "invalid-cover");
    return std::nullopt;
  }
  if (!(m.cover.begin <= hEff.aStart && hEff.aEnd <= m.cover.end)) {
    traceDirectCalleeReject(
        m, "hunk-not-inside-cover",
        llvm::formatv("cover=[{0},{1}) hEffA=[{2},{3})", m.cover.begin,
                      m.cover.end, hEff.aStart, hEff.aEnd)
            .str());
    return std::nullopt;
  }
  // Do not require the seed hunk itself to be body-owned.  The whole-cover
  // orchestrator invokes this phase once per normalized hunk, and a mixed edit
  // may present an argument hunk before the body hunk that motivated the
  // alternate callee.  The body-edit obligation is discharged below by proving
  // that the original definition replayed with B actuals no longer matches B,
  // while a unique alternate definition does match B.

  std::optional<std::pair<uint64_t, uint64_t>> calleeByteRange =
      findDirectCalleeNameByteRange(invSpanText, m);
  if (!calleeByteRange) {
    traceDirectCalleeReject(m, "callee-byte-range-not-found",
                            stringutils::showWsWithClip(invSpanText, 220));
    return std::nullopt;
  }
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "callee-range inv id={0} name={1} bytes=[{2},{3})",
                   m.id, m.name, calleeByteRange->first,
                   calleeByteRange->second);

  std::optional<ProducerFunctionMacroDefinition> currentDef =
      activeFunctionDefinitionBefore(deps_.model, m.id, m.name);
  if (!currentDef) {
    traceDirectCalleeReject(m, "current-definition-not-active");
    return std::nullopt;
  }
  if (m.definitionDirectiveId &&
      currentDef->directive->id != *m.definitionDirectiveId) {
    traceDirectCalleeReject(
        m, "current-definition-id-mismatch",
        llvm::formatv("active={0} producer={1}", currentDef->directive->id,
                      *m.definitionDirectiveId)
            .str());
    return std::nullopt;
  }
  REFOLD_LOG_TRACE(
      "macro/direct-callee",
      "current-definition inv id={0} name={1} directive id={2} params={3} "
      "replacementTokens={4}",
      m.id, m.name, currentDef->directive->id,
      currentDef->directive->defParams.size(),
      currentDef->directive->replacementTokens.size());

  std::optional<MacroArgumentTokenSpellings> aActualArgs =
      recoverActualArgumentSpellingsFromArgSpans(m, deps_.aToks);
  if (!aActualArgs) {
    traceDirectCalleeReject(
        m, "a-actual-argument-spellings-not-recovered",
        llvm::formatv("defParams={0} argSpans={1} aTokCount={2}",
                      m.defParams.size(), m.argSpans.size(),
                      deps_.aToks.size())
            .str());
    return std::nullopt;
  }
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "a-actuals-recovered inv id={0} name={1} count={2}",
                   m.id, m.name, aActualArgs->size());

  std::optional<BActualArgumentReplaySurfaces> bActualSurfaces =
      recoverBActualArgumentReplaySurfaces(m, deps_.sourceMapper, deps_.bToks);
  if (!bActualSurfaces) {
    traceDirectCalleeReject(
        m, "b-actual-argument-spellings-not-recovered",
        llvm::formatv("defParams={0} argSpans={1} bTokCount={2}",
                      m.defParams.size(), m.argSpans.size(),
                      deps_.bToks.size())
            .str());
    return std::nullopt;
  }
  MacroArgumentTokenSpellings bActualArgs =
      tokenSpellingsFromBActualSurfaces(*bActualSurfaces);
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "b-actuals-recovered inv id={0} name={1} count={2}",
                   m.id, m.name, bActualArgs.size());

  std::optional<MacroTokenSpellings> currentExpansion =
      replayProducerFunctionMacroTokens(*currentDef, *aActualArgs);
  if (!currentExpansion) {
    traceDirectCalleeReject(m, "current-definition-replay-failed");
    return std::nullopt;
  }
  if (!deps_.tokenSpellingsEqualToA(*currentExpansion, m.cover.begin,
                                    m.cover.end)) {
    traceDirectCalleeReject(
        m, "current-definition-a-replay-mismatch",
        llvm::formatv("replayedTokens={0} cover=[{1},{2})",
                      currentExpansion->size(), m.cover.begin, m.cover.end)
            .str());
    return std::nullopt;
  }
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "current-definition-a-replay-ok inv id={0} name={1} "
                   "tokens={2} cover=[{3},{4})",
                   m.id, m.name, currentExpansion->size(), m.cover.begin,
                   m.cover.end);

  std::optional<std::pair<size_t, size_t>> bEnv =
      deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(m.cover.begin,
                                                       m.cover.end);
  if (!bEnv || bEnv->second <= bEnv->first) {
    traceDirectCalleeReject(
        m, "b-envelope-not-found",
        llvm::formatv("cover=[{0},{1}) hasEnvelope={2}", m.cover.begin,
                      m.cover.end, bEnv ? 1 : 0)
            .str());
    return std::nullopt;
  }
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "b-envelope inv id={0} name={1} B=[{2},{3})", m.id,
                   m.name, bEnv->first, bEnv->second);

  // If the original callee, replayed under the edited B-side actuals, already
  // explains the B cover, then the observed edit is args-only and direct callee
  // substitution would be a gratuitous rename.  The path is only for covers
  // whose body-owned surface requires selecting another active definition.
  std::optional<MacroTokenSpellings> currentBExpansion =
      replayProducerFunctionMacroTokens(*currentDef, bActualArgs);
  if (!currentBExpansion) {
    traceDirectCalleeReject(m, "current-definition-b-replay-failed");
    return std::nullopt;
  }
  if (deps_.tokenSpellingsEqualToB(*currentBExpansion,
                                   static_cast<uint64_t>(bEnv->first),
                                   static_cast<uint64_t>(bEnv->second))) {
    traceDirectCalleeReject(m, "current-definition-already-replays-b");
    return std::nullopt;
  }

  std::optional<std::vector<std::pair<size_t, size_t>>> baseArgRanges =
      deps_.getMacroInvocationFormalArgContentRanges(m, invSpanText);
  if (!baseArgRanges) {
    traceDirectCalleeReject(m, "base-arg-ranges-not-parsed",
                            stringutils::showWsWithClip(invSpanText, 220));
    return std::nullopt;
  }
  REFOLD_LOG_TRACE("macro/direct-callee",
                   "base-arg-ranges inv id={0} name={1} count={2}", m.id,
                   m.name, baseArgRanges->size());

  DirectCalleeSubstitutionTraceStats stats;
  SmallVector<DirectCalleeSubstitutionCandidate, 4> candidates;
  for (const RefoldModel::MacroDirective &directive :
       deps_.model.GetMacroDirectives()) {
    ++stats.definitionsScanned;
    std::optional<ProducerFunctionMacroDefinition> candidateDef =
        producerFunctionDefinitionFromDirective(directive);
    if (!candidateDef) {
      ++stats.nonFunctionDefinitions;
      continue;
    }
    if (candidateDef->name == m.name) {
      ++stats.originalName;
      traceDirectCalleeCandidateReject(m, directive, "original-callee-name");
      continue;
    }

    REFOLD_LOG_TRACE(
        "macro/direct-callee",
        "candidate-consider inv id={0} name={1} directive id={2} "
        "candidate={3} params={4} replacementTokens={5}",
        m.id, m.name, directive.id, directive.name,
        directive.defParams.size(), directive.replacementTokens.size());

    std::optional<ProducerFunctionMacroDefinition> activeCandidate =
        activeFunctionDefinitionBefore(deps_.model, m.id, candidateDef->name);
    if (!activeCandidate ||
        activeCandidate->directive->id != candidateDef->directive->id) {
      ++stats.inactiveDefinitions;
      traceDirectCalleeCandidateReject(m, directive, "not-active-before-call");
      continue;
    }
    if (candidateDef->directive->defParams.size() !=
        currentDef->directive->defParams.size()) {
      ++stats.arityMismatches;
      traceDirectCalleeCandidateReject(m, directive, "arity-mismatch");
      continue;
    }

    std::optional<MacroTokenSpellings> candidateExpansion =
        replayProducerFunctionMacroTokens(*candidateDef, bActualArgs);
    if (!candidateExpansion) {
      ++stats.replayFailures;
      traceDirectCalleeCandidateReject(m, directive, "candidate-replay-failed");
      continue;
    }
    if (!deps_.tokenSpellingsEqualToB(*candidateExpansion,
                                      static_cast<uint64_t>(bEnv->first),
                                      static_cast<uint64_t>(bEnv->second))) {
      ++stats.bReplayMismatches;
      REFOLD_LOG_TRACE(
          "macro/direct-callee",
          "candidate-reject inv id={0} name={1} directive id={2} "
          "candidate={3} reason=b-replay-mismatch replayedTokens={4} "
          "B=[{5},{6})",
          m.id, m.name, directive.id, directive.name,
          candidateExpansion->size(), bEnv->first, bEnv->second);
      continue;
    }

    // The direct proof is permitted to edit only the spelling of the callsite
    // callee token plus producer-identified argument content slots.  This lets
    // mixed edits compose the alternate callee with ordinary argument edits,
    // but still rejects any unmodelled change to delimiters, suffixes,
    // whitespace outside argument slots, or other fixed invocation syntax.
    std::optional<std::string> replacement = buildDirectCalleeReplacementText(
        invSpanText, *calleeByteRange, candidateDef->name, *baseArgRanges,
        *bActualSurfaces);
    if (!replacement) {
      ++stats.shapeFailures;
      REFOLD_LOG_TRACE(
          "macro/direct-callee",
          "candidate-reject inv id={0} name={1} directive id={2} "
          "candidate={3} reason=callee-arg-shape-failed",
          m.id, m.name, directive.id, directive.name);
      continue;
    }

    if (StringRef(*replacement) == invSpanText) {
      ++stats.noopReplacements;
      traceDirectCalleeCandidateReject(m, directive, "noop-replacement");
      continue;
    }

    ++stats.accepted;
    REFOLD_LOG_TRACE(
        "macro/direct-callee",
        "candidate-accept inv id={0} name={1} directive id={2} candidate={3} "
        "replacement='{4}' B=[{5},{6})",
        m.id, m.name, directive.id, directive.name,
        stringutils::showWsWithClip(*replacement, 220), bEnv->first,
        bEnv->second);
    candidates.push_back(DirectCalleeSubstitutionCandidate{
        candidateDef->directive->id, static_cast<uint64_t>(bEnv->first),
        static_cast<uint64_t>(bEnv->second),
        static_cast<uint64_t>(candidateDef->name.size()),
        std::move(*replacement)});
  }

  if (candidates.empty()) {
    traceDirectCalleeSearchSummary(m, stats, 0);
    traceDirectCalleeReject(m, "no-accepted-alternate-callee");
    return std::nullopt;
  }

  llvm::sort(candidates, directCalleeSubstitutionCandidateLess);

  const std::string &chosenReplacement = candidates.front().replacement;
  unsigned replacementClasses = 1;
  for (size_t candidateIdx = 1; candidateIdx < candidates.size();
       ++candidateIdx) {
    if (candidates[candidateIdx].replacement !=
        candidates[candidateIdx - 1].replacement)
      ++replacementClasses;
  }
  traceDirectCalleeSearchSummary(m, stats, replacementClasses);

  for (const DirectCalleeSubstitutionCandidate &candidate : candidates) {
    if (candidate.replacement != chosenReplacement) {
      traceDirectCalleeReject(
          m, "ambiguous-alternate-callee-replacements",
          llvm::formatv("chosen='{0}' other='{1}'",
                        stringutils::showWsWithClip(chosenReplacement, 220),
                        stringutils::showWsWithClip(candidate.replacement, 220))
              .str());
      return std::nullopt;
    }
  }

  MacroPatch patch{*m.invB, *m.invE, chosenReplacement, m.id};
  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = candidates.front().bTokStart;
  patch.materialized.bTokEnd = candidates.front().bTokEnd;
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart = calleeByteRange->first;
  patch.materialized.outputByteEnd =
      calleeByteRange->first + candidates.front().newCalleeSize;
  MacroPatchProof proof = deps_.proofLattice.MakeMacroPatchProof(
      MacroPatchProofKind::DirectCalleeSubstitution,
      /*preservesInvocationStructure=*/true, m.id);
  WholeEnvelopeReplayWitness wholeEnvelopeWitness;
  wholeEnvelopeWitness.rootMacroId = m.id;
  wholeEnvelopeWitness.replayValidated = true;
  wholeEnvelopeWitness.definitionTapeReplayValidated = true;
  proof.wholeEnvelopeReplay = wholeEnvelopeWitness;
  deps_.proofLattice.SetMacroPatchProof(patch, std::move(proof));
  REFOLD_LOG_TRACE(
      "macro/direct-callee",
      "built-patch inv id={0} name={1} replacement='{2}' invBytes=[{3},{4}) "
      "B=[{5},{6}) proofKind={7}",
      m.id, m.name, stringutils::showWsWithClip(patch.replacement, 220),
      patch.invRange.begin, patch.invRange.end, patch.materialized.bTokStart,
      patch.materialized.bTokEnd, toString(patch.proof.kind));
  return patch;
}

std::optional<MacroPatch> RefoldMacroSelectorSubstitutionPhase::Run(
    const RefoldMacroWholeCoverPlanningContext &planningCtx) const {
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &hEff = planningCtx.hEff;
  StringRef baseInvText = planningCtx.baseInvText;
  [[maybe_unused]] const uint64_t invStart = planningCtx.invStart;
  [[maybe_unused]] const uint64_t invEnd = planningCtx.invEnd;

  StringRef invSpanText =
      !baseInvText.empty()
          ? baseInvText
          : (m.invText ? StringRef(*m.invText) : StringRef(""));
  REFOLD_LOG_TRACE(
      "macro/selector-substitution",
      "enter inv id={0} name={1} subkind={2} hEffA=[{3},{4}) hEffB=[{5},{6}) "
      "baseInvTextBytes={7} invTextBytes={8} invB={9} invE={10} text='{11}'",
      m.id, m.name, m.subkind, hEff.aStart, hEff.aEnd, hEff.bStart,
      hEff.bEnd, baseInvText.size(), m.invText ? m.invText->size() : 0,
      formatOptionalU64(m.invB), formatOptionalU64(m.invE),
      stringutils::showWsWithClip(invSpanText, 220));
  if (m.subkind != "func") {
    REFOLD_LOG_TRACE("macro/selector-substitution",
                     "reject inv id={0} name={1} reason=not-function-like",
                     m.id, m.name);
    return std::nullopt;
  }
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          invSpanText, m)) {
    REFOLD_LOG_TRACE(
        "macro/selector-substitution",
        "reject inv id={0} name={1} reason=callsite-prefix-mismatch text='{2}'",
        m.id, m.name, stringutils::showWsWithClip(invSpanText, 220));
    return std::nullopt;
  }
  if (!m.invB || !m.invE) {
    REFOLD_LOG_TRACE(
        "macro/selector-substitution",
        "reject inv id={0} name={1} reason=missing-invocation-byte-range invB={2} invE={3}",
        m.id, m.name, formatOptionalU64(m.invB), formatOptionalU64(m.invE));
    return std::nullopt;
  }

  if (std::optional<MacroPatch> directCalleePatch =
          TryDirectCalleeSubstitution(planningCtx, invSpanText)) {
    REFOLD_LOG_TRACE(
        "macro/selector-substitution",
        "direct-callee-accepted inv id={0} name={1} replacement='{2}' proof={3}",
        m.id, m.name,
        stringutils::showWsWithClip(directCalleePatch->replacement, 220),
        toString(directCalleePatch->proof.kind));
    return directCalleePatch;
  }
  REFOLD_LOG_TRACE("macro/selector-substitution",
                   "direct-callee-declined inv id={0} name={1}", m.id,
                   m.name);

  auto rootArgRangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(m, invSpanText);
  if (!rootArgRangesOpt) {
    REFOLD_LOG_TRACE(
        "macro/selector-substitution",
        "reject inv id={0} name={1} reason=root-arg-ranges-not-parsed",
        m.id, m.name);
    return std::nullopt;
  }
  const auto &rootArgRanges = *rootArgRangesOpt;

  // Recover the selected callee's ordinary argument token sequence from the
  // A-side PP cover by subtracting producer-recorded body spans.  This
  // keeps the replay proof entirely token-based: every non-body occurrence
  // of the single formal must have the same A-token spelling sequence, or
  // the proof has no unique non-selector argument vector and rejects.
  auto coverMinusBodySingleArg = [&](const RefoldModel::MacroInvocation &leaf)
      -> std::optional<MacroArgumentTokenSpellings> {
    if (leaf.defParams.size() != 1 || !leaf.cover.IsValid() ||
        leaf.cover.begin >= leaf.cover.end ||
        leaf.cover.end > deps_.aToks.size())
      return std::nullopt;

    SmallVector<RefoldModel::PPSpan, 4> body;
    body.append(leaf.bodySpans.begin(), leaf.bodySpans.end());
    llvm::sort(body, ppSpanLessByTokenRange);

    auto collectGapTokens =
        [&](uint64_t beginTok,
            uint64_t endTok) -> std::optional<MacroTokenSpellings> {
      if (endTok < beginTok || endTok > deps_.aToks.size())
        return std::nullopt;
      MacroTokenSpellings out;
      for (uint64_t tok = beginTok; tok < endTok; ++tok)
        out.push_back(deps_.aToks[static_cast<size_t>(tok)].spelling);
      return out;
    };

    uint64_t cursor = leaf.cover.begin;
    std::optional<MacroTokenSpellings> argTokens;
    for (const auto &sp : body) {
      if (sp.end <= leaf.cover.begin || sp.begin >= leaf.cover.end)
        continue;
      const uint64_t clippedBegin =
          std::max<uint64_t>(sp.begin, leaf.cover.begin);
      const uint64_t clippedEnd = std::min<uint64_t>(sp.end, leaf.cover.end);
      if (cursor < clippedBegin) {
        std::optional<MacroTokenSpellings> gap =
            collectGapTokens(cursor, clippedBegin);
        if (!gap)
          return std::nullopt;
        if (!gap->empty()) {
          if (argTokens && !tokenSpellingVectorsEqual(*argTokens, *gap))
            return std::nullopt;
          argTokens = std::move(*gap);
        }
      }
      cursor = std::max<uint64_t>(cursor, clippedEnd);
    }
    if (cursor < leaf.cover.end) {
      std::optional<MacroTokenSpellings> gap =
          collectGapTokens(cursor, leaf.cover.end);
      if (!gap)
        return std::nullopt;
      if (!gap->empty()) {
        if (argTokens && !tokenSpellingVectorsEqual(*argTokens, *gap))
          return std::nullopt;
        argTokens = std::move(*gap);
      }
    }
    if (!argTokens)
      return std::nullopt;

    MacroArgumentTokenSpellings out;
    out.push_back(std::move(*argTokens));
    return out;
  };

  // Build an invocation index so provenance walks can climb from the
  // descendant callee back to the root invocation deterministically by
  // producer-recorded caller ids.
  DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
  for (const auto &mi : deps_.model.GetMacroInvocations())
    invById[mi.id] = &mi;

  auto depthToRoot =
      [&](const RefoldModel::MacroInvocation &cand) -> std::optional<unsigned> {
    unsigned d = 0;
    std::optional<uint64_t> p = cand.callerMacroId;
    while (p) {
      ++d;
      if (*p == m.id)
        return d;
      auto it = invById.find(*p);
      if (it == invById.end())
        break;
      p = it->second->callerMacroId;
    }
    return std::nullopt;
  };

  // A selector witness identifies the exact root invocation byte slice that
  // supplied the paste-derived callee selector, plus the replacement
  // spelling that would select a candidate active macro definition.  Byte
  // offsets are relative to the root invocation text used for the emitted
  // MacroPatch.
  struct SelectorRewriteWitness {
    uint32_t rootArgIdx = 0;
    uint64_t selectorByteBegin = 0;
    uint64_t selectorByteEnd = 0;
    std::string oldSelector;
    std::string newSelector;
  };

  // Invert the producer-recorded callee-origin tape against a candidate
  // macro name.  Literal parts must match exactly.  Exactly one
  // caller_arg_slice part may supply the selector, and it must name this
  // root invocation and a concrete byte slice of one root argument.  If the
  // split against the next literal anchor is ambiguous, the proof rejects.
  auto deriveSelectorRewriteForCandidateName =
      [&](const RefoldModel::MacroCalleeOrigin &origin,
          StringRef candidateName) -> std::optional<SelectorRewriteWitness> {
    if (origin.kind != MacroCalleeOriginKind::Paste || !origin.spelling ||
        *origin.spelling == candidateName || origin.parts.empty() ||
        candidateName.empty())
      return std::nullopt;

    size_t candidatePos = 0;
    std::optional<SelectorRewriteWitness> selector;
    for (size_t i = 0; i < origin.parts.size(); ++i) {
      const RefoldModel::CalleeOriginPart &part = origin.parts[i];
      if (part.kind == RefoldModel::CalleeOriginPartKind::Literal) {
        if (!candidateName.substr(candidatePos).starts_with(part.spelling))
          return std::nullopt;
        candidatePos += part.spelling.size();
        continue;
      }

      if (part.kind != RefoldModel::CalleeOriginPartKind::CallerArgSlice ||
          !part.rootMacroId || *part.rootMacroId != m.id ||
          !part.rootParamIndex || !part.byteBegin || !part.byteEnd ||
          *part.rootParamIndex >= rootArgRanges.size())
        return std::nullopt;
      if (selector)
        return std::nullopt;

      const auto &argRange = rootArgRanges[*part.rootParamIndex];
      const uint64_t argLen = argRange.second - argRange.first;
      if (*part.byteEnd < *part.byteBegin || *part.byteEnd > argLen)
        return std::nullopt;

      StringRef rootSlice = invSpanText.slice(argRange.first + *part.byteBegin,
                                              argRange.first + *part.byteEnd);
      if (rootSlice != part.spelling)
        return std::nullopt;

      StringRef nextLiteral;
      for (size_t j = i + 1; j < origin.parts.size(); ++j) {
        if (origin.parts[j].kind ==
            RefoldModel::CalleeOriginPartKind::Literal) {
          nextLiteral = origin.parts[j].spelling;
          break;
        }
        if (origin.parts[j].kind ==
            RefoldModel::CalleeOriginPartKind::CallerArgSlice)
          return std::nullopt;
      }

      StringRef replacementPart;
      if (nextLiteral.empty()) {
        replacementPart = candidateName.drop_front(candidatePos);
        candidatePos = candidateName.size();
      } else {
        size_t found = candidateName.find(nextLiteral, candidatePos);
        if (found == StringRef::npos)
          return std::nullopt;
        if (candidateName.find(nextLiteral, found + 1) != StringRef::npos)
          return std::nullopt;
        replacementPart = candidateName.slice(candidatePos, found);
        candidatePos = found;
      }

      if (replacementPart.empty() || replacementPart == part.spelling)
        return std::nullopt;

      SelectorRewriteWitness out;
      out.rootArgIdx = *part.rootParamIndex;
      out.selectorByteBegin = argRange.first + *part.byteBegin;
      out.selectorByteEnd = argRange.first + *part.byteEnd;
      out.oldSelector = part.spelling.str();
      out.newSelector = replacementPart.str();
      selector = std::move(out);
    }

    if (candidatePos != candidateName.size() || !selector)
      return std::nullopt;
    return selector;
  };

  // Each candidate records one complete explanation of B: which descendant
  // callee was edited, which active alternate macro definition explains the
  // B-side cover, and what root invocation text would select that macro.
  struct SelectorCandidate {
    uint64_t leafId = 0;
    uint64_t candidateDirectiveId = 0;
    uint32_t rootArgIdx = 0;
    uint64_t selectorByteBegin = 0;
    uint64_t selectorByteEnd = 0;
    uint64_t bTokStart = 0;
    uint64_t bTokEnd = 0;
    uint64_t newSelectorSize = 0;
    std::string replacement;
  };
  SmallVector<SelectorCandidate, 4> candidates;

  // Search descendants of the current root for the selected callee whose
  // body-owned tokens contain the edited A hunk.  This prevents selector
  // substitution from firing on unrelated paste tokens in the same root
  // DAG.
  for (const auto &leaf : deps_.model.GetMacroInvocations()) {
    if (leaf.id == m.id || leaf.subkind != "func")
      continue;
    std::optional<unsigned> depth = depthToRoot(leaf);
    if (!depth || *depth == 0)
      continue;
    if (!leaf.cover.IsValid() ||
        !(leaf.cover.begin <= hEff.aStart && hEff.aEnd <= leaf.cover.end) ||
        !hunkWithinPPSpans(hEff, leaf.bodySpans))
      continue;

    // First prove that the original selected callee definition explains
    // the A-side cover under the recovered non-selector arguments.  Without
    // this baseline equality, replacing the selector would be relating B to
    // a model that did not actually produce A.
    std::optional<ProducerFunctionMacroDefinition> currentDef =
        activeFunctionDefinitionBefore(deps_.model, leaf.id, leaf.name);
    if (!currentDef ||
        (leaf.definitionDirectiveId &&
         currentDef->directive->id != *leaf.definitionDirectiveId))
      continue;
    std::optional<MacroArgumentTokenSpellings> actualArgs =
        coverMinusBodySingleArg(leaf);
    if (!actualArgs)
      continue;

    std::optional<MacroTokenSpellings> currentExpansion =
        replayProducerFunctionMacroTokens(*currentDef, *actualArgs);
    if (!currentExpansion ||
        !deps_.tokenSpellingsEqualToA(*currentExpansion, leaf.cover.begin,
                                      leaf.cover.end))
      continue;

    // The alternate macro must explain exactly the B token envelope mapped
    // from the selected callee's original PP cover.  The selector proof
    // does not widen the edit or borrow neighboring B tokens.
    std::optional<std::pair<size_t, size_t>> bEnv =
        deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(leaf.cover.begin,
                                                         leaf.cover.end);
    if (!bEnv || bEnv->second <= bEnv->first)
      continue;

    if (leaf.calleeOrigin.kind != MacroCalleeOriginKind::Paste ||
        !leaf.calleeOrigin.spelling ||
        *leaf.calleeOrigin.spelling != leaf.name ||
        leaf.calleeOrigin.parts.empty())
      continue;

    // Enumerate existing macro definitions as possible selector targets.
    // This is a finite namespace proof over definitions already present in
    // the source.  Each candidate must be active at the root expansion
    // point and must replay through producer-recorded replacement tokens.
    for (const auto &directive : deps_.model.GetMacroDirectives()) {
      std::optional<ProducerFunctionMacroDefinition> candidateDef =
          producerFunctionDefinitionFromDirective(directive);
      if (!candidateDef || candidateDef->name == leaf.name)
        continue;
      std::optional<ProducerFunctionMacroDefinition> activeCandidate =
          activeFunctionDefinitionBefore(deps_.model, m.id, candidateDef->name);
      if (!activeCandidate ||
          activeCandidate->directive->id != candidateDef->directive->id)
        continue;
      if (candidateDef->directive->defParams.size() !=
          currentDef->directive->defParams.size())
        continue;

      std::optional<SelectorRewriteWitness> selector =
          deriveSelectorRewriteForCandidateName(leaf.calleeOrigin,
                                                candidateDef->name);
      if (!selector)
        continue;

      std::optional<MacroTokenSpellings> candidateExpansion =
          replayProducerFunctionMacroTokens(*candidateDef, *actualArgs);
      if (!candidateExpansion ||
          !deps_.tokenSpellingsEqualToB(*candidateExpansion,
                                        static_cast<uint64_t>(bEnv->first),
                                        static_cast<uint64_t>(bEnv->second)))
        continue;

      // Build the only source edit admitted by this proof: replace the
      // producer-proven selector slice inside the root invocation and leave
      // the rest of the callsite unchanged.  The normal root replay
      // validator still checks that the resulting invocation text is
      // well-formed.
      std::string replacement = invSpanText.str();
      replacement.replace(selector->selectorByteBegin,
                          selector->selectorByteEnd -
                              selector->selectorByteBegin,
                          selector->newSelector);
      if (!deps_.validateMergedDirectAndDagRootReplacement(m, invSpanText,
                                                           replacement))
        continue;

      candidates.push_back(SelectorCandidate{
          leaf.id, candidateDef->directive->id, selector->rootArgIdx,
          selector->selectorByteBegin, selector->selectorByteEnd,
          static_cast<uint64_t>(bEnv->first),
          static_cast<uint64_t>(bEnv->second),
          static_cast<uint64_t>(selector->newSelector.size()),
          std::move(replacement)});
    }
  }

  if (candidates.empty())
    return std::nullopt;

  // Sort before uniqueness checking so diagnostics and tie handling are
  // deterministic.  The proof accepts multiple witnesses only when they all
  // lead to the exact same root replacement text; distinct selector
  // rewrites are treated as ambiguous and rejected.
  llvm::sort(candidates,
             [](const SelectorCandidate &lhs, const SelectorCandidate &rhs) {
               if (lhs.replacement != rhs.replacement)
                 return lhs.replacement < rhs.replacement;
               if (lhs.leafId != rhs.leafId)
                 return lhs.leafId < rhs.leafId;
               if (lhs.candidateDirectiveId != rhs.candidateDirectiveId)
                 return lhs.candidateDirectiveId < rhs.candidateDirectiveId;
               if (lhs.rootArgIdx != rhs.rootArgIdx)
                 return lhs.rootArgIdx < rhs.rootArgIdx;
               if (lhs.selectorByteBegin != rhs.selectorByteBegin)
                 return lhs.selectorByteBegin < rhs.selectorByteBegin;
               if (lhs.selectorByteEnd != rhs.selectorByteEnd)
                 return lhs.selectorByteEnd < rhs.selectorByteEnd;
               if (lhs.bTokStart != rhs.bTokStart)
                 return lhs.bTokStart < rhs.bTokStart;
               return lhs.bTokEnd < rhs.bTokEnd;
             });

  const std::string &chosenReplacement = candidates.front().replacement;
  for (const SelectorCandidate &candidate : candidates)
    if (candidate.replacement != chosenReplacement) {
      return std::nullopt;
    }

  // Certify the accepted selector substitution as a structure-preserving root
  // macro patch.  The materialized B token range records the descendant
  // expansion that this selector explains, while the output byte range
  // points at the rewritten root argument inside the replacement callsite
  // text.
  MacroPatch patch{*m.invB, *m.invE, chosenReplacement, m.id};
  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = candidates.front().bTokStart;
  patch.materialized.bTokEnd = candidates.front().bTokEnd;
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart = candidates.front().selectorByteBegin;
  patch.materialized.outputByteEnd =
      candidates.front().selectorByteBegin + candidates.front().newSelectorSize;
  deps_.proofLattice.SetMacroPatchProof(
      patch, deps_.proofLattice.MakeMacroPatchProof(
                 MacroPatchProofKind::PasteDerivedCalleeSelector,
                 /*preservesInvocationStructure=*/true, m.id));
  return patch;
}

} // namespace refold
} // namespace clang
