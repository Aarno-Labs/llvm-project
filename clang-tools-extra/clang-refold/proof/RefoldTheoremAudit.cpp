//===--- RefoldTheoremAudit.cpp --------------------------------*- C++ -*-===//
//
// Central theorem/audit ledger and legacy-authority audit service.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldLog.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldTerminalProofSink.h"
#include "util/StringUtils.h"

#include "llvm/Support/FormatVariadic.h"

#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldTheoremAudit::RefoldTheoremAudit(
    TheoremAuditStats &audit, const RefoldTerminalProofSink &terminalSink,
    bool strict, Hooks hooks)
    : audit_(audit), terminalSink_(terminalSink), strict_(strict),
      hooks_(std::move(hooks)) {}

bool RefoldTheoremAudit::IsNoLegacyAuditEnabled() const {
  // The no-legacy audit no longer depends on temporary environment-variable
  // controls. It is now a regular strict/theorem invariant: non-strict runs do
  // not emit diagnostic-only audit noise, and strict runs always fail closed if
  // a legacy-authority seam reaches a theorem boundary.
  return strict_;
}

LegacyAuditEvidence
RefoldTheoremAudit::MakeLegacyAuditEvidence(LegacyPathKind kind,
                                            StringRef role, StringRef detail) {
  LegacyAuditEvidence evidence;
  evidence.kind = kind;
  evidence.role = role.str();
  evidence.detail = detail.str();
  return evidence;
}

void RefoldTheoremAudit::NoteTheoremAuditViolation(StringRef detail) const {
  if (audit_.theoremSatisfied)
    audit_.firstViolation = detail.str();
  audit_.theoremSatisfied = false;
}

void RefoldTheoremAudit::ReportNoLegacyAuditFinding(
    const LegacyAuditEvidence &evidence) const {
  if (!IsNoLegacyAuditEnabled())
    return;

  ++audit_.noLegacyAuditFindings;

  const LegacyPathDefinition definition = describeLegacyPathKind(evidence.kind);
  const StringRef role =
      evidence.role.empty() ? StringRef("<unknown>") : StringRef(evidence.role);
  const std::string clippedDetail =
      evidence.detail.empty()
          ? std::string()
          : stringutils::showWsWithClip(evidence.detail, 240);

  if (clippedDetail.empty()) {
    REFOLD_LOG_WARN("no-legacy-audit",
         "kind={0} role={1} definition=\"{2}\" required-closure=\"{3}\"",
         definition.kind, role, definition.definition,
         definition.requiredClosure);
  } else {
    REFOLD_LOG_WARN("no-legacy-audit",
         "kind={0} role={1} definition=\"{2}\" required-closure=\"{3}\" "
         "detail=\"{4}\"",
         definition.kind, role, definition.definition,
         definition.requiredClosure, clippedDetail);
  }

  // No-legacy findings are theorem state whenever the strict/theorem guard is
  // active. Record the finding here so every audit site shares one policy and
  // cannot accidentally remain a diagnostic-only report.
  NoteTheoremAuditViolation(
      llvm::formatv("strict/theorem no-legacy audit reported finding: "
                    "kind={0} role={1} detail={2}",
                    definition.kind, role,
                    clippedDetail.empty() ? StringRef("<none>")
                                          : StringRef(clippedDetail))
          .str());
}

void RefoldTheoremAudit::EnforceTheoremAuditInvariants() const {
  // The theorem audit is authoritative in strict mode: any emitted non-terminal
  // result that is not declared, explicit-proof-backed, locally discharged,
  // lattice-resolved, and in-domain violates the declared theorem domain and
  // therefore forces terminal fallback in strict mode.
  if (audit_.selectorDirectBypasses != 0) {
    NoteTheoremAuditViolation(
        "selector resolved an emitted result through a direct bypass");
  }
  if (audit_.emittedSelectorOnlyExceptionCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit relied on selector-only exception carrier");
  }
  if (audit_.emittedTransitionalTheoremCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit carried transitional theorem-facing artifact");
  }
  if (audit_.emittedUnknownClassCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit carried unknown-class theorem artifact");
  }
  if (audit_.emittedUndischargedCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit carried undischarged theorem artifact");
  }
  if (audit_.emittedOutOfDomainCarriers != 0) {
    NoteTheoremAuditViolation("non-terminal emitted edit carried explicit "
                              "out-of-domain theorem artifact");
  }
  if (audit_.emittedUncomposedCompositeEdits != 0) {
    NoteTheoremAuditViolation(
        "emitted composite edit lacked an ordered proof composition");
  }
  if (audit_.selectorUnresolvedCompetitions != 0) {
    NoteTheoremAuditViolation(
        "selector competition ended without a lattice-selected winner");
  }
  if (audit_.nonExplicitTerminalExclusions != 0) {
    NoteTheoremAuditViolation("terminal fallback was not classified as an "
                              "explicit out-of-domain theorem result");
  }
  if (audit_.stateTransitionAuditViolations != 0) {
    NoteTheoremAuditViolation(
        "state-transition gateway audit found an undischarged or malformed "
        "state proof");
  }
  if (hooks_.getWitnessResolverMode &&
      hooks_.getWitnessResolverMode() == WitnessResolverMode::Strict) {
    if (audit_.resolverPotentiallyMissingProof != 0) {
      NoteTheoremAuditViolation(
          "strict resolver audit found a potentially in-domain missing proof");
    }
    if (audit_.resolverUnknownDomain != 0) {
      NoteTheoremAuditViolation(
          "strict resolver audit found an unclassified domain outcome");
    }
    if (audit_.resolverStrictLegacyFallback != 0) {
      NoteTheoremAuditViolation(
          "strict resolver audit found a legacy fallback");
    }
    if (audit_.resolverStrictInvalidFailClosed != 0) {
      NoteTheoremAuditViolation(
          "strict resolver audit found a fail-closed outcome that was not "
          "complete non-equivalent ambiguity");
    }
    if (audit_.resolverDeclaredIncompleteKeys != 0 ||
        audit_.resolverDeclaredIncompatibleComposition != 0 ||
        audit_.resolverDeclaredUnconvertedWitnesses != 0) {
      NoteTheoremAuditViolation(
          "strict resolver audit found a declared in-domain repair without "
          "complete keys, converted witnesses, and compatible composition");
    }
    if (audit_.resolverClosureLedgerRows != 0) {
      NoteTheoremAuditViolation(
          "strict resolver audit emitted missing-proof closure ledger rows");
    }
  }
  if (IsNoLegacyAuditEnabled() && audit_.noLegacyAuditFindings != 0) {
    NoteTheoremAuditViolation(
        llvm::formatv("strict/theorem no-legacy audit produced {0} finding(s)",
                      audit_.noLegacyAuditFindings)
            .str());
  }

  if (!strict_ || terminalSink_.HasRequest() || audit_.theoremSatisfied)
    return;

  terminalSink_.RequestTerminalFallback(
      MakeTerminalFallbackProofFailure(
          TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
          TerminalFallbackFailureReason::TheoremAuditInvariantViolation),
      "theorem", BuildTheoremAuditInvariantDetail());
}

void RefoldTheoremAudit::RecordWitnessResolverTheoremAudit(
    const WitnessResolverDecision &decision) const {
  using DomainClass = WitnessStrictDomainClass;

  ++audit_.resolverDomainAudits;
  audit_.resolverClosureLedgerRows += decision.closureLedger.size();

  switch (decision.strictDomain.domainClass) {
  case DomainClass::DeclaredInDomain:
    ++audit_.resolverDeclaredInDomain;
    break;
  case DomainClass::ExplicitOutOfDomain:
    ++audit_.resolverExplicitOutOfDomain;
    break;
  case DomainClass::AmbiguousOutOfDomain:
    ++audit_.resolverAmbiguousOutOfDomain;
    break;
  case DomainClass::PotentiallyInDomainMissingProof:
    ++audit_.resolverPotentiallyMissingProof;
    break;
  case DomainClass::Unknown:
    ++audit_.resolverUnknownDomain;
    break;
  }

  if (decision.mode != WitnessResolverMode::Strict)
    return;

  const bool strictResolverAuthority =
      decision.strictUseResolver && decision.resolverIndex.has_value();
  const bool strictLegacyFallback =
      !strictResolverAuthority && !decision.strictFailClosed;

  if (strictResolverAuthority)
    ++audit_.resolverStrictResolverAuthority;
  if (strictLegacyFallback)
    ++audit_.resolverStrictLegacyFallback;
  if (decision.strictFailClosed)
    ++audit_.resolverStrictFailClosed;

  auto reject = [&](StringRef why) {
    NoteTheoremAuditViolation(
        llvm::formatv("strict resolver theorem audit rejected role={0}: {1}; "
                      "domain={2} fallback_class={3} reason={4}",
                      decision.role, why,
                      decision.strictDomain.domainClass,
                      decision.fallbackClass,
                      decision.failureReason.empty()
                          ? StringRef("<none>")
                          : StringRef(decision.failureReason))
            .str());
  };

  if (decision.strictDomain.domainClass ==
      DomainClass::PotentiallyInDomainMissingProof) {
    reject("resolver still has a potentially in-domain missing proof");
  }

  if (decision.strictDomain.domainClass == DomainClass::Unknown)
    reject("resolver outcome has no strict-domain classification");

  if (!decision.closureLedger.empty())
    reject("resolver emitted missing-proof closure ledger rows");

  if (strictLegacyFallback)
    reject("strict mode fell back to legacy selection");

  if (decision.strictDomain.domainClass == DomainClass::DeclaredInDomain) {
    if (!strictResolverAuthority)
      reject("declared in-domain resolver outcome was not authoritative");

    if (decision.incompleteWitnessCount != 0) {
      ++audit_.resolverDeclaredIncompleteKeys;
      reject(
          "declared in-domain resolver outcome used incomplete witness keys");
    }

    if (decision.resolverUnconvertedWitnessCount != 0) {
      ++audit_.resolverDeclaredUnconvertedWitnesses;
      reject("declared in-domain resolver outcome used unconverted witnesses");
    }

    if (!decision.composition.compatible ||
        decision.composition.incompleteTupleCount != 0 ||
        decision.composition.incompatibleTupleCount != 0) {
      ++audit_.resolverDeclaredIncompatibleComposition;
      reject(
          "declared in-domain resolver outcome lacked compatible composition");
    }
  }

  if (decision.strictFailClosed) {
    const bool completeNonEquivalentAmbiguity =
        decision.strictDomain.domainClass ==
            DomainClass::AmbiguousOutOfDomain &&
        decision.incompleteWitnessCount == 0 &&
        decision.resolverUnconvertedWitnessCount == 0 &&
        (decision.fallbackClass ==
             WitnessFallbackClass::MultipleNonEquivalentWitnessClasses ||
         (decision.fallbackClass == WitnessFallbackClass::CompositionFailure &&
          decision.composition.failureIsFatal &&
          decision.composition.incompleteTupleCount == 0));

    if (!completeNonEquivalentAmbiguity) {
      ++audit_.resolverStrictInvalidFailClosed;
      reject("strict fail-closed outcome was not complete non-equivalent "
             "ambiguity");
    }
  }
}


void RefoldTheoremAudit::RecordDirectStateCheckClosure(
    DirectStateCheckKind checkKind, OwnerStateComponent component,
    DirectStateCheckClosureKind closure, StateMutationKind mutation,
    StringRef stage, StringRef detail) const {
  if (!IsNoLegacyAuditEnabled())
    return;

  ++audit_.directStateChecksAudited;
  switch (closure) {
  case DirectStateCheckClosureKind::OwnerStateDeltaFact:
    ++audit_.directStateChecksDeltaFacts;
    break;
  case DirectStateCheckClosureKind::OwnerStateGraphEdge:
    ++audit_.directStateChecksGraphEdges;
    break;
  case DirectStateCheckClosureKind::StateTransitionGatewayWitness:
    ++audit_.directStateChecksGatewayWitnesses;
    break;
  case DirectStateCheckClosureKind::TerminalFallbackProofFailure:
    ++audit_.directStateChecksTerminalFailures;
    break;
  case DirectStateCheckClosureKind::Unknown:
  case DirectStateCheckClosureKind::ExplicitlyUnclosedLocalCheck:
    ++audit_.directStateChecksUnclosedLocal;
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::StateCheckOutsideGateway, stage,
        llvm::formatv("direct state check is not routed through a theorem "
                      "closure surface: check={0} component={1} mutation={2} "
                      "closure={3} detail={4}",
                      checkKind, component, mutation, closure, detail)
            .str()));
    break;
  }
}

void RefoldTheoremAudit::RecordOwnerStateGraphAudit(
    const OwnerStateGraphAuditStats &audit) const {
  audit_.graphOwnerNodes = audit.ownerNodes;
  audit_.graphZeroTokenStateNodes = audit.zeroTokenStateNodes;
  audit_.graphObservedStateComponents = audit.observedStateComponents;
  audit_.graphMutatedStateComponents = audit.mutatedStateComponents;
  audit_.graphIncomparableNodes = audit.incomparableNodes;
  audit_.graphMissingProducerFacts = audit.missingProducerFacts;
}

void RefoldTheoremAudit::RecordGraphIncomparableNode() const {
  ++audit_.graphIncomparableNodes;
}

void RefoldTheoremAudit::RecordStateTransitionGatewayCheck() const {
  ++audit_.stateTransitionGatewayChecks;
}

void RefoldTheoremAudit::RecordStateTransitionGatewayStable() const {
  ++audit_.stateTransitionGatewayStable;
}

void RefoldTheoremAudit::RecordStateTransitionGatewayTerminalFailure() const {
  ++audit_.stateTransitionGatewayTerminalFailures;
}

void RefoldTheoremAudit::RecordStateTransitionUnknownComponentViolation() const {
  ++audit_.stateTransitionUnknownComponentViolations;
}

void RefoldTheoremAudit::RecordStateTransitionUnknownMutationViolation() const {
  ++audit_.stateTransitionUnknownMutationViolations;
}

void RefoldTheoremAudit::RecordStateTransitionNoneWitnessViolation() const {
  ++audit_.stateTransitionNoneWitnessViolations;
}

void RefoldTheoremAudit::RecordStateTransitionGatewayViolation(
    StringRef detail) const {
  ++audit_.stateTransitionAuditViolations;
  NoteTheoremAuditViolation(detail);
}

bool RefoldTheoremAudit::AuditProofSummaryForLegacyAuthority(
    const ProofSummary &summary, StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  const bool hasAcceptedPath =
      summary.inventory.currentPath != AcceptedPathKind::Unknown;
  const bool hasFinalTheoremClass =
      summary.theoremClass != TheoremProofClass::Unknown;

  if (hasAcceptedPath && !hasFinalTheoremClass) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PathSpecificProofMirror, role,
        llvm::formatv(
            "proof summary has accepted path {0} but no final theorem class",
            summary.inventory.currentPath)
            .str()));
  }

  if (hasFinalTheoremClass && !summary.primaryProofClassExplicit) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PostSummaryProofKindDecision, role,
        llvm::formatv("theorem class {0} is present but "
                      "primaryProofClassExplicit is false",
                      summary.theoremClass)
            .str()));
  }

  if (summary.structurePreserving &&
      (!hasFinalTheoremClass ||
       summary.inventory.support !=
           AcceptanceSupportKind::ExplicitProofBacked)) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::StructurePreservingProofBit, role,
        llvm::formatv(
            "structurePreserving survived without an explicit proof-backed "
            "summary: theoremClass={0} support={1} path={2}",
            summary.theoremClass, summary.inventory.support,
            summary.inventory.currentPath)
            .str()));
  }

  if (summary.theoremClass == TheoremProofClass::TerminalOutOfDomainProof) {
    if (!summary.terminalFallbackWitness) {
      ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
          LegacyPathKind::UnclassifiedFallbackBranch, role,
          "terminal accepted class has no TerminalFallbackWitness"));
    } else {
      const TerminalFallbackWitness &witness = *summary.terminalFallbackWitness;
      if (const TerminalFallbackProofFailure *primary = witness.PrimaryFailure())
        AuditTerminalFallbackForLegacyAuthority(*primary, role);
      else
        ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
            LegacyPathKind::UnclassifiedFallbackBranch, role,
            "terminal accepted class has an empty TerminalFallbackWitness"));
      for (const TerminalFallbackProofFailure &failure : witness.proofFailures)
        AuditTerminalFallbackForLegacyAuthority(failure, role);
    }
  }

  return true;
}

bool RefoldTheoremAudit::AuditAcceptedResultCandidateForLegacyAuthority(
    const AcceptedResultCandidate &candidate, StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  if (candidate.kind == AcceptedResultCandidateKind::Unknown) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PathSpecificProofMirror, role,
        "accepted artifact has no AcceptedResultCandidate kind"));
    return true;
  }

  const EmissionPathKind primaryPath =
      RefoldProofLattice::PrimaryEmissionPathForCandidateKind(candidate.kind);
  if (!candidate.emissionPaths.Contains(primaryPath)) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PathSpecificProofMirror, role,
        llvm::formatv(
            "accepted candidate kind {0} is not reflected in the accepted-result "
            "emission-path inventory; expected primary path {1}",
            candidate.kind, primaryPath)
            .str()));
  }

  if ((candidate.proofSummary.hasMixedOwnerTilingWitness ||
       candidate.proofSummary.theoremClass ==
           TheoremProofClass::MixedOwnerTilingProof) &&
      !candidate.emissionPaths.Contains(
          EmissionPathKind::MixedOwnerTilingSegment)) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PathSpecificProofMirror, role,
        "mixed-owner tiling witness is not represented in the accepted-result "
        "emission-path inventory"));
  }

  if ((candidate.proofSummary.hasOwnerRealizationWitness ||
       candidate.proofSummary.theoremClass ==
           TheoremProofClass::OwnerRealizationProof) &&
      !candidate.emissionPaths.Contains(
          EmissionPathKind::OwnerRealizationMaterialization)) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PathSpecificProofMirror, role,
        "owner-realization witness is not represented in the accepted-result "
        "emission-path inventory"));
  }

  AuditProofSummaryForLegacyAuthority(candidate.proofSummary, role);

  std::optional<TheoremProofClass> theoremClass;
  if (hooks_.normalizeAcceptedProof)
    theoremClass = hooks_.normalizeAcceptedProof(candidate);
  if (!theoremClass) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PostSummaryProofKindDecision, role,
        llvm::formatv(
            "accepted candidate kind={0} path={1} theoremClass={2} did not "
            "normalize to one final TheoremProofClass",
            candidate.kind, candidate.proofSummary.inventory.currentPath,
            candidate.proofSummary.theoremClass)
            .str()));
  }

  if (candidate.kind == AcceptedResultCandidateKind::TerminalOutOfDomain ||
      candidate.proofSummary.theoremClass ==
          TheoremProofClass::TerminalOutOfDomainProof) {
    if (!candidate.proofSummary.terminalFallbackWitness) {
      ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
          LegacyPathKind::UnclassifiedFallbackBranch, role,
          "terminal candidate has no TerminalFallbackWitness"));
    } else {
      const TerminalFallbackWitness &witness =
          *candidate.proofSummary.terminalFallbackWitness;
      if (const TerminalFallbackProofFailure *primary = witness.PrimaryFailure())
        AuditTerminalFallbackForLegacyAuthority(*primary, role);
      else
        ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
            LegacyPathKind::UnclassifiedFallbackBranch, role,
            "terminal candidate has an empty TerminalFallbackWitness"));
      for (const TerminalFallbackProofFailure &failure : witness.proofFailures)
        AuditTerminalFallbackForLegacyAuthority(failure, role);
    }
  }

  return true;
}

bool RefoldTheoremAudit::AuditMacroPatchProofForLegacyAuthority(
    const MacroPatch &patch, StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  if (patch.proof.kind == MacroPatchProofKind::Unknown &&
      !patch.proof.preservesInvocationStructure)
    return true;

  const ProofSummary expected = hooks_.classifyMacroPatchProof
                                    ? hooks_.classifyMacroPatchProof(patch)
                                    : ProofSummary{};
  const bool missingSummary =
      patch.proofSummary.theoremClass == TheoremProofClass::Unknown &&
      patch.proofSummary.inventory.currentPath == AcceptedPathKind::Unknown;
  const bool staleSummary =
      patch.proofSummary.theoremClass != expected.theoremClass ||
      patch.proofSummary.inventory.currentPath != expected.inventory.currentPath ||
      patch.proofSummary.structurePreserving != expected.structurePreserving ||
      patch.proofSummary.proofRootMacroId != expected.proofRootMacroId;

  if (missingSummary || staleSummary) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PathSpecificProofMirror, role,
        llvm::formatv(
            "MacroPatch proof summary is {0}: proofKind={1} "
            "preservesInvocationStructure={2} proofRootMacroId={3} "
            "summaryPath={4} expectedPath={5} summaryTheoremClass={6} "
            "expectedTheoremClass={7}",
            missingSummary ? StringRef("missing") : StringRef("stale"),
            patch.proof.kind, patch.proof.preservesInvocationStructure ? 1 : 0,
            patch.proof.proofRootMacroId,
            patch.proofSummary.inventory.currentPath,
            expected.inventory.currentPath, patch.proofSummary.theoremClass,
            expected.theoremClass)
            .str()));
  }

  if (patch.proof.preservesInvocationStructure &&
      expected.theoremClass != TheoremProofClass::InvocationPreservingProof) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::StructurePreservingProofBit, role,
        llvm::formatv(
            "MacroPatch preservesInvocationStructure=true does not classify as "
            "InvocationPreservingProof: proofKind={0} theoremClass={1}",
            patch.proof.kind, expected.theoremClass)
            .str()));
  }

  return true;
}

bool RefoldTheoremAudit::AuditTerminalFallbackForLegacyAuthority(
    const TerminalFallbackProofFailure &failure, StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  if (!IsClassifiedTerminalFallbackProofFailure(failure)) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::UnclassifiedFallbackBranch, role,
        llvm::formatv("terminal fallback lacks classified proof failure: {0}",
                      failure)
            .str()));
  }

  return true;
}

bool RefoldTheoremAudit::AuditExpansionFallbackBranchClassification(
    const ExpansionFallbackBranchClassification &classification,
    StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  if (!classification.IsClassified()) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::UnclassifiedFallbackBranch, role,
        llvm::formatv(
            "expansion fallback branch {0} is not classified onto exactly one "
            "proof-lattice class: branchProofClass={1} theoremClass={2} "
            "acceptedPath={3}",
            classification.branch, classification.branchProofClass,
            classification.theoremClass, classification.acceptedPath)
            .str()));
  }

  return true;
}

bool RefoldTheoremAudit::AuditExpansionFallbackAcceptedCandidate(
    const ExpansionFallbackBranchClassification &classification,
    const AcceptedResultCandidate &candidate, StringRef role) const {
  if (!IsNoLegacyAuditEnabled())
    return true;

  AuditExpansionFallbackBranchClassification(classification, role);
  AuditAcceptedResultCandidateForLegacyAuthority(candidate, role);

  if (!classification.IsClassified())
    return true;

  if (candidate.proofSummary.inventory.currentPath !=
      classification.acceptedPath) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::UnclassifiedFallbackBranch, role,
        llvm::formatv(
            "expansion fallback branch {0} declared accepted path {1} but "
            "emitted candidate path {2}",
            classification.branch, classification.acceptedPath,
            candidate.proofSummary.inventory.currentPath)
            .str()));
  }

  if (candidate.proofSummary.theoremClass != classification.theoremClass) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::UnclassifiedFallbackBranch, role,
        llvm::formatv(
            "expansion fallback branch {0} declared theorem class {1} via "
            "branch proof class {2} but emitted candidate theorem class {3}",
            classification.branch, classification.theoremClass,
            classification.branchProofClass,
            candidate.proofSummary.theoremClass)
            .str()));
  }

  return true;
}

bool RefoldTheoremAudit::AuditTerminalFallbackProofFailure(
    const TerminalFallbackProofFailure &failure, StringRef role) const {
  AuditTerminalFallbackForLegacyAuthority(failure, role);

  bool ok = true;
  auto reject = [&](StringRef reason) {
    ok = false;
    ++audit_.terminalFailureAuditViolations;
    NoteTheoremAuditViolation(
        llvm::formatv("terminal fallback proof failure audit rejected {0}: {1}; "
                      "failure={2}",
                      role, reason, failure)
            .str());
  };

  if (!IsClassifiedTerminalFallbackProofFailure(failure)) {
    reject("failure was Unknown or Unclassified");
    return false;
  }

  // A generic MissingProducerFacts reason is intentionally allowed only when
  // the structured context identifies what producer fact is missing.  More
  // specific local reasons that normalize to MissingProducerFacts already name
  // the failed fact in their enum value (for example an unmappable include
  // B-envelope), so requiring additional context for those would create a new
  // fallback surface instead of improving theorem precision.
  if (failure.reason == TerminalFallbackFailureReason::MissingProducerFacts &&
      failure.context.Empty()) {
    reject("generic MissingProducerFacts did not identify the missing fact");
  }

  const std::optional<TheoremFallbackFailureKind> normalized =
      NormalizeTerminalFallbackFailureReason(failure.reason);
  if (!normalized || *normalized != failure.theoremFailure)
    reject("theorem failure kind did not match the normalized local reason");

  return ok;
}

void RefoldTheoremAudit::RecordTerminalFallbackProofFailureListMissing(
    StringRef detail) const {
  ++audit_.terminalFailureAuditViolations;
  NoteTheoremAuditViolation(detail);
}

bool RefoldTheoremAudit::RejectNoLegacyAuditFindingIfStrict(
    const LegacyAuditEvidence &evidence,
    const TerminalFallbackProofFailure &failure) const {
  if (!IsNoLegacyAuditEnabled())
    return false;

  ReportNoLegacyAuditFinding(evidence);
  ++audit_.noLegacyEmissionBoundaryViolations;
  ++audit_.noLegacyStrictRejections;
  const LegacyPathDefinition definition = describeLegacyPathKind(evidence.kind);
  const StringRef role =
      evidence.role.empty() ? StringRef("<unknown>") : StringRef(evidence.role);
  const std::string detail =
      evidence.detail.empty()
          ? std::string("<none>")
          : stringutils::showWsWithClip(evidence.detail, 200);
  NoteTheoremAuditViolation(
      llvm::formatv(
          "strict/theorem no-legacy audit rejected emission boundary: "
          "kind={0} role={1} detail={2}",
          definition.kind, role, detail)
          .str());

  if (!terminalSink_.HasRequest()) {
    terminalSink_.RequestTerminalFallback(
        failure, role,
        llvm::formatv(
            "strict/theorem no-legacy audit rejected emission boundary: "
            "kind={0} detail={1}",
            definition.kind, detail)
            .str());
  }
  return true;
}

TerminalFallbackProofFailure
RefoldTheoremAudit::MakeMissingSelectedMacroPatchCarrierFailure() const {
  return MakeTerminalFallbackProofFailure(
      TerminalFallbackObligationKind::EmissionArtifactDischarged,
      TerminalFallbackFailureReason::UndischargedEmissionArtifact,
      TerminalFallbackFailureContext::ForStateComponent(
          "MacroPatch.selectedAcceptedCandidate"));
}

bool RefoldTheoremAudit::RejectMissingSelectedMacroPatchCarrier(
    const MacroPatch &patch, StringRef role, StringRef detail) const {
  const TerminalFallbackProofFailure failure =
      MakeMissingSelectedMacroPatchCarrierFailure();

  const bool strictAuditRejected = RejectNoLegacyAuditFindingIfStrict(
      MakeLegacyAuditEvidence(LegacyPathKind::PathSpecificProofMirror, role,
                              detail),
      failure);

  // Missing selectedAcceptedCandidate is no longer recoverable proof state.
  // Strict engine runs must fail closed instead of rebuilding authority from
  // the raw MacroPatch.
  if (!strictAuditRejected) {
    NoteTheoremAuditViolation(
        llvm::formatv("macro patch bytes=[{0},{1}) reached emission without "
                      "MacroPatch.selectedAcceptedCandidate: {2}",
                      patch.invStart, patch.invEnd,
                      detail.empty() ? StringRef("<none>") : detail)
            .str());
    if (strict_ && !terminalSink_.HasRequest()) {
      terminalSink_.RequestTerminalFallback(
          failure, role,
          llvm::formatv("macro patch bytes=[{0},{1}) reached emission without "
                        "MacroPatch.selectedAcceptedCandidate: {2}",
                        patch.invStart, patch.invEnd,
                        detail.empty() ? StringRef("<none>") : detail)
              .str());
    }
  }

  return strictAuditRejected || strict_;
}

bool RefoldTheoremAudit::AuditStateTransitionGatewayProofs(
    StringRef emissionStage, StringRef emissionOwner) const {
  if (audit_.stateTransitionAuditViolations == 0)
    return true;

  ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
      LegacyPathKind::StateCheckOutsideGateway, emissionStage,
      llvm::formatv(
          "state-transition gateway recorded violations before emission: "
          "owner={0} violations={1} unknownComponents={2} "
          "unknownMutations={3} noneWitnesses={4}",
          emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner,
          audit_.stateTransitionAuditViolations,
          audit_.stateTransitionUnknownComponentViolations,
          audit_.stateTransitionUnknownMutationViolations,
          audit_.stateTransitionNoneWitnessViolations)
          .str()));

  const StringRef owner =
      emissionOwner.empty() ? StringRef("<unknown>") : emissionOwner;
  NoteTheoremAuditViolation(
      llvm::formatv("state-transition gateway audit rejected emission in {0}: "
                    "checks={1} stable={2} terminalFailures={3} "
                    "violations={4} unknownComponents={5} "
                    "unknownMutations={6} noneWitnesses={7}",
                    owner, audit_.stateTransitionGatewayChecks,
                    audit_.stateTransitionGatewayStable,
                    audit_.stateTransitionGatewayTerminalFailures,
                    audit_.stateTransitionAuditViolations,
                    audit_.stateTransitionUnknownComponentViolations,
                    audit_.stateTransitionUnknownMutationViolations,
                    audit_.stateTransitionNoneWitnessViolations)
          .str());

  if (strict_ && !terminalSink_.HasRequest()) {
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
            TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
            TerminalFallbackFailureContext::ForStateComponent(
                "StateTransitionGateway")),
        emissionStage,
        llvm::formatv("state-transition gateway audit failed before emission "
                      "for {0}",
                      owner)
            .str());
  }
  return false;
}

void RefoldTheoremAudit::RecordTerminalFallbackTheoremAudit() const {
  if (!terminalSink_.HasRequest())
    return;

  const TerminalFallbackWitness witness = hooks_.buildTerminalFallbackWitness();
  const AcceptedResultCandidate candidate =
      hooks_.buildAcceptedTerminalCandidate(witness);
  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "RecordTerminalFallbackTheoremAudit");
  const ProofSummary &summary = candidate.proofSummary;

  audit_.terminalFailureObligations += witness.proofFailures.size();
  if (witness.proofFailures.size() > 1)
    audit_.terminalSecondaryFailureObligations +=
        witness.proofFailures.size() - 1;

  bool structuredFailuresClassified = !witness.proofFailures.empty();
  for (const TerminalFallbackProofFailure &failure : witness.proofFailures)
    structuredFailuresClassified &=
        IsClassifiedTerminalFallbackProofFailure(failure);
  const TerminalFallbackProofFailure *primaryFailure = witness.PrimaryFailure();
  // The terminal exclusion must remain explicit all the way through the
  // normalized proof carrier. Merely requesting fallback is not sufficient; the
  // resulting terminal witness must classify as the named explicit out-of-domain
  // theorem result.
  const bool explicitTerminalCarrier =
      candidate.kind == AcceptedResultCandidateKind::TerminalOutOfDomain &&
      summary.theoremClass == TheoremProofClass::TerminalOutOfDomainProof &&
      summary.inventory.currentPath ==
          AcceptedPathKind::TerminalEmitEditedPreprocessedStream &&
      summary.inventory.support ==
          AcceptanceSupportKind::ExplicitOutOfDomainClass &&
      summary.completeness.coverage ==
          CompletenessCoverageKind::ExplicitOutOfDomainClass &&
      summary.theoremDomain.kind ==
          TheoremDomainKind::ExplicitOutOfDomainClass &&
      summary.theoremDomain.hasExplicitExclusion &&
      summary.theoremDomain.explicitExclusion !=
          TheoremFallbackFailureKind::Unknown &&
      primaryFailure &&
      summary.theoremDomain.explicitExclusion ==
          primaryFailure->theoremFailure &&
      summary.terminalFallbackWitness.has_value() &&
      IsClassifiedTerminalFallbackProofFailure(*primaryFailure) &&
      structuredFailuresClassified &&
      summary.discharge.status == ProofDischargeStatus::Rejected &&
      summary.discharge.failedObligation ==
          ProofObligationKind::ExplicitOutOfDomainResultTracked &&
      summary.discharge.failureReason ==
          ProofFailureReason::ExplicitOutOfDomainResult;

  if (explicitTerminalCarrier) {
    ++audit_.explicitTerminalExclusions;
    return;
  }

  ++audit_.nonExplicitTerminalExclusions;
  NoteTheoremAuditViolation(
      llvm::formatv("terminal fallback escaped theorem-domain classification: "
                    "witness={0} candidateKind={1}",
                    witness, candidate.kind)
          .str());
}

std::string RefoldTheoremAudit::BuildTheoremAuditInvariantDetail() const {
  const std::string firstViolation =
      audit_.firstViolation.empty()
          ? std::string("<none>")
          : stringutils::showWsWithClip(audit_.firstViolation, 200);

  return llvm::formatv(
             "strict theorem-audit invariant violation: firstViolation='{0}' "
             "selectorDirectBypasses={1} selectorOnlyExceptions={2} "
             "transitional={3} undischarged={4} unknownClass={5} "
             "outOfDomain={6} "
             "uncomposedComposite={7} selectorUnresolved={8} "
             "nonExplicitTerminalExclusions={9} "
             "terminalFailureAuditViolations={10} "
             "stateTransitionAuditViolations={11} noLegacyFindings={12} "
             "noLegacyEmissionViolations={13} noLegacyStrictRejections={14} "
             "directStateChecks={15} directStateUnclosed={16} "
             "resolverMissingProof={17} resolverUnknownDomain={18} "
             "strictLegacyFallback={19} strictInvalidFailClosed={20} "
             "declaredIncompleteKeys={21} declaredIncompatibleComposition={22} "
             "declaredUnconverted={23} closureLedgerRows={24}",
             firstViolation, audit_.selectorDirectBypasses,
             audit_.emittedSelectorOnlyExceptionCarriers,
             audit_.emittedTransitionalTheoremCarriers,
             audit_.emittedUndischargedCarriers,
             audit_.emittedUnknownClassCarriers,
             audit_.emittedOutOfDomainCarriers,
             audit_.emittedUncomposedCompositeEdits,
             audit_.selectorUnresolvedCompetitions,
             audit_.nonExplicitTerminalExclusions,
             audit_.terminalFailureAuditViolations,
             audit_.stateTransitionAuditViolations,
             audit_.noLegacyAuditFindings,
             audit_.noLegacyEmissionBoundaryViolations,
             audit_.noLegacyStrictRejections,
             audit_.directStateChecksAudited,
             audit_.directStateChecksUnclosedLocal,
             audit_.resolverPotentiallyMissingProof,
             audit_.resolverUnknownDomain,
             audit_.resolverStrictLegacyFallback,
             audit_.resolverStrictInvalidFailClosed,
             audit_.resolverDeclaredIncompleteKeys,
             audit_.resolverDeclaredIncompatibleComposition,
             audit_.resolverDeclaredUnconvertedWitnesses,
             audit_.resolverClosureLedgerRows)
      .str();
}

} // namespace refold
} // namespace clang
