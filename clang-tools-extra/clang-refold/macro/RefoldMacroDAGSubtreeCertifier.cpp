//===--- RefoldMacroDAGSubtreeCertifier.cpp -------------------*- C++ -*-===//
//
// Subtree semantic certifier for macro DAG lifting.
//
// This translation unit owns the cross-node proof checks that determine whether
// a lifted root rewrite is semantically compatible with the validated macro
// subtree.  It combines text primitives, structured-lift certificates, formal
// interaction consistency, deferred paste discharge, and subtree semantic
// admissibility into the certificates consumed by the final DAG candidate
// validator.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDAGSubtreeCertifier.h"

#include "core/RefoldLog.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroDAGSubtreeCertifier::RefoldMacroDAGSubtreeCertifier(
    Dependencies deps)
    : deps_(std::move(deps)) {}

SubtreeInteractionSummaryCertificate
RefoldMacroDAGSubtreeCertifier::BuildSubtreeInteractionSummaryCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    ArrayRef<SemanticInteractionCertificate> interactions) const {
  SubtreeInteractionSummaryCertificate cert;
  cert.interactions.assign(interactions.begin(), interactions.end());

  // Fold each per-interaction certificate into subtree-wide feature bits.
  // The generic booleans record whether a mechanism appears anywhere,
  // while the switch records the important combined modes where that
  // mechanism interacts with paste or with other mixed semantics.
  for (const auto &interaction : interactions) {
    cert.hasPaste |= interaction.touchesPaste;
    cert.hasStringify |= interaction.usesStringify;
    cert.hasWideStringify |= interaction.usesWideStringify;
    cert.hasRawInvocation |= interaction.usesRawInvocationPreservation;
    cert.hasPreferredChildSyntax |= interaction.usesPreferredChildSyntax;
    switch (interaction.kind) {
    case SemanticInteractionKind::Plain:
    case SemanticInteractionKind::ChildSyntax:
    case SemanticInteractionKind::RawInvocation:
    case SemanticInteractionKind::Stringify:
    case SemanticInteractionKind::WideStringify:
    case SemanticInteractionKind::Paste:
      break;
    case SemanticInteractionKind::ChildSyntaxPaste:
      cert.hasChildSyntaxPaste = true;
      break;
    case SemanticInteractionKind::RawInvocationPaste:
      cert.hasRawInvocationPaste = true;
      break;
    case SemanticInteractionKind::StringifyPaste:
      cert.hasStringifyPaste = true;
      break;
    case SemanticInteractionKind::WideStringifyPaste:
      cert.hasStringifyPaste = true;
      cert.hasWideStringifyPaste = true;
      break;
    case SemanticInteractionKind::Mixed:
      cert.hasMixedInteractions = true;
      break;
    }
  }

  cert.detail =
      formatv(
          "subtree interaction summary: interactions={0} "
          "paste={1} stringify={2} wide={3} rawInvocation={4} "
          "childSyntax={5} stringifyPaste={6} "
          "wideStringifyPaste={7} rawInvocationPaste={8} "
          "childSyntaxPaste={9} mixed={10}",
          cert.interactions.size(), cert.hasPaste ? 1 : 0,
          cert.hasStringify ? 1 : 0, cert.hasWideStringify ? 1 : 0,
          cert.hasRawInvocation ? 1 : 0, cert.hasPreferredChildSyntax ? 1 : 0,
          cert.hasStringifyPaste ? 1 : 0, cert.hasWideStringifyPaste ? 1 : 0,
          cert.hasRawInvocationPaste ? 1 : 0, cert.hasChildSyntaxPaste ? 1 : 0,
          cert.hasMixedInteractions ? 1 : 0)
          .str();
  return cert;
}

SubtreeInteractionConsistencyCertificate
RefoldMacroDAGSubtreeCertifier::BuildSubtreeInteractionConsistencyCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    ArrayRef<FormalRewriteCertificate> formalCertificates) const {
  SubtreeInteractionConsistencyCertificate cert;
  StringMap<size_t> keyToIndex;

  // Key by logical formal identity, not by certificate position. The same
  // invocation argument may appear through several lifted rewrite paths.
  auto makeKey = [&](const RefoldModel::MacroInvocation *inv,
                     uint32_t argIdx) -> std::string {
    return formatv("{0}#{1}", inv ? inv->id : 0, argIdx).str();
  };

  for (const auto &formalCert : formalCertificates) {
    cert.formalConsistencies.push_back(formalCert.interactionConsistency);
    const auto &consistency = cert.formalConsistencies.back();

    // A formal-level invalidity is already a failed proof obligation, so
    // the subtree consistency certificate must fail immediately.
    if (!consistency.valid) {
      cert.valid = false;
      cert.failure =
          SubtreeInteractionConsistencyFailure::DivergentFormalSemantics;
      cert.detail = consistency.detail;
      return cert;
    }

    std::string key = makeKey(consistency.inv, consistency.argIdx);
    auto it = keyToIndex.find(key);
    if (it == keyToIndex.end()) {
      keyToIndex[key] = cert.formalConsistencies.size() - 1;
      continue;
    }

    // Duplicate evidence for the same logical formal is acceptable only
    // if the semantic signature is exactly the same. Otherwise two paths
    // are asking the same formal to be interpreted under different macro
    // semantics, which is not a sound merge.
    const auto &existing = cert.formalConsistencies[it->second];
    if (!(existing.signature == consistency.signature)) {
      cert.valid = false;
      cert.failure =
          SubtreeInteractionConsistencyFailure::DivergentFormalSemantics;
      cert.detail =
          formatv("subtree semantic interaction consistency "
                  "failed: inv id={0} argIdx={1} formal semantic "
                  "evidence diverged across lift/root paths",
                  consistency.inv ? consistency.inv->id : 0, consistency.argIdx)
              .str();
      return cert;
    }
  }

  cert.detail = formatv("subtree interaction consistency: formals={0} "
                        "uniqueFormals={1}",
                        cert.formalConsistencies.size(), keyToIndex.size())
                    .str();
  return cert;
}

DeferredPasteDischargeCertificate
RefoldMacroDAGSubtreeCertifier::BuildSubtreeDeferredPasteDischargeCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const SubtreeSemanticCertificate &semantic,
    const InvocationRewriteCertificate &rootCert) const {
  DeferredPasteDischargeCertificate cert;

  // Walk caller links to determine whether `ancestor` dominates
  // `descendant` in the macro invocation DAG. Missing metadata is treated
  // as non-ancestry rather than guessed.
  auto isAncestorOrSame =
      [&](const RefoldModel::MacroInvocation *ancestor,
          const RefoldModel::MacroInvocation *descendant) -> bool {
    if (!ancestor || !descendant)
      return false;
    const RefoldModel::MacroInvocation *cur = descendant;
    while (cur) {
      if (cur->id == ancestor->id)
        return true;
      if (!cur->callerMacroId)
        break;
      auto it =
          ctx.subtreeValidationCtx.invocationById.find(*cur->callerMacroId);
      if (it == ctx.subtreeValidationCtx.invocationById.end())
        break;
      cur = it->second;
    }
    return false;
  };

  // A deferred paste can also be discharged by the accepted root replay
  // when the path from the deferred invocation to the root used a lexical
  // bridge. In that case, require every bridge-derived root formal to
  // appear verbatim in the accepted root rewrite certificate.
  auto rootReplayMatchesLexicalBridgeChain =
      [&](const InvocationRewriteCertificate &deferredInvCert) -> bool {
    if (!deferredInvCert.inv || !rootCert.inv ||
        !rootCert.pasteValidation.valid)
      return false;

    for (const auto &liftCert : semantic.liftChains) {
      if (!liftCert.leaf)
        continue;
      if (!isAncestorOrSame(deferredInvCert.inv, liftCert.leaf))
        continue;
      if (!liftCert.usedLexicalBridge || liftCert.bridgedRootArgIdxs.empty())
        continue;

      bool allBridgedRootFormalsMatched = true;
      for (uint32_t rootArgIdx : liftCert.bridgedRootArgIdxs) {
        auto rootFormalIt = liftCert.rootFormals.find(rootArgIdx);
        if (rootFormalIt == liftCert.rootFormals.end()) {
          allBridgedRootFormalsMatched = false;
          break;
        }

        bool matchedRewrite = false;
        for (const auto &rewrite : rootCert.rewrites) {
          if (rewrite.argIdx != rootArgIdx)
            continue;
          if (rewrite.oldText == rootFormalIt->second.oldText &&
              rewrite.newText == rootFormalIt->second.newText) {
            matchedRewrite = true;
            break;
          }
        }
        if (!matchedRewrite) {
          allBridgedRootFormalsMatched = false;
          break;
        }
      }

      if (allBridgedRootFormalsMatched)
        return true;
    }
    return false;
  };

  for (const auto &invCert : semantic.invocationCertificates) {
    if (!invCert.pasteValidation.deferred)
      continue;
    cert.deferredInvocations.push_back(&invCert);

    // Semantic discharge means an ancestor formal certificate used a mode
    // strong enough to preserve the paste-sensitive input without relying
    // only on local paste-shape validation.
    bool hasSemanticDischarge = false;
    for (const auto &formalCert : semantic.formalCertificates) {
      if (!isAncestorOrSame(formalCert.inv, invCert.inv))
        continue;
      const auto &sig = formalCert.interactionConsistency.signature;
      if (sig.usesPreferredChildSyntax || sig.usesRawInvocationPreservation ||
          sig.usesStringify || sig.usesWideStringify ||
          sig.usesRawChildInvocationLogicalInput) {
        hasSemanticDischarge = true;
        break;
      }
    }

    // Ancestor replay discharge means some strictly outer invocation has
    // already validated a paste replay that covers this deferred inner
    // obligation.
    bool hasAncestorReplayPath = false;
    for (const auto &candidate : semantic.invocationCertificates) {
      if (!candidate.pasteValidation.valid)
        continue;
      if (!isAncestorOrSame(candidate.inv, invCert.inv))
        continue;
      if (candidate.inv && invCert.inv && candidate.inv->id == invCert.inv->id)
        continue;
      hasAncestorReplayPath = true;
      break;
    }

    // The root replay is a valid discharge either when the deferred
    // invocation is the root itself, or when the bridge-derived root
    // formals prove that the accepted root rewrite consumed the bridged
    // syntax exactly.
    const bool hasAcceptedRootReplayCandidate =
        (invCert.inv && rootCert.inv && invCert.inv->id == rootCert.inv->id &&
         rootCert.pasteValidation.valid) ||
        rootReplayMatchesLexicalBridgeChain(invCert);

    // Every deferred paste must have an explicit discharge witness. Do
    // not allow a deferred local proof to leak into the accepted subtree
    // as an unstated global assumption.
    if (!(hasSemanticDischarge || hasAncestorReplayPath ||
          hasAcceptedRootReplayCandidate)) {
      cert.valid = false;
      cert.failure =
          hasSemanticDischarge
              ? DeferredPasteDischargeFailure::MissingAncestorPasteValidation
              : DeferredPasteDischargeFailure::MissingSemanticDischarge;
      cert.detail =
          formatv("subtree deferred paste discharge failed: inv "
                  "id={0} name={1} has deferred paste validation "
                  "without ancestor semantic discharge, "
                  "accepted ancestor replay path, or accepted "
                  "root replay candidate",
                  invCert.inv ? invCert.inv->id : 0,
                  invCert.inv ? invCert.inv->name : StringRef("<none>"))
              .str();
      return cert;
    }
  }

  cert.detail = formatv("subtree deferred paste discharge: deferredInvs={0}",
                        cert.deferredInvocations.size())
                    .str();
  return cert;
}

SubtreeSemanticAdmissibilityCertificate
RefoldMacroDAGSubtreeCertifier::BuildSubtreeSemanticAdmissibilityCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const SubtreeSemanticCertificate &semantic) const {
  SubtreeSemanticAdmissibilityCertificate cert;

  // Deferred paste validation is allowed only if a later semantic or
  // ancestor replay witness discharged it. Otherwise the subtree would be
  // accepted with an unresolved paste-shape obligation.
  if (!semantic.deferredPasteDischarge.valid) {
    cert.valid = false;
    cert.failure =
        SubtreeSemanticAdmissibilityFailure::DeferredPasteNotDischarged;
    cert.detail = semantic.deferredPasteDischarge.detail;
    return cert;
  }

  // Mixed interactions are rejected at the subtree level because they
  // indicate that the same accepted subtree would need incompatible
  // semantic interpretations, such as wrapper/stringify/paste behavior
  // that cannot be folded into one sound structural witness.
  if (semantic.interactionSummary.hasMixedInteractions) {
    cert.valid = false;
    cert.failure =
        SubtreeSemanticAdmissibilityFailure::MixedSemanticInteractions;
    cert.detail =
        "subtree semantic admissibility failed: mixed wrapper/"
        "stringify/paste interactions are not structurally admissible";
    return cert;
  }

  // Classify the sensitive semantic features that make a lexical bridge
  // or flattening step dangerous. These summary booleans keep the later
  // admissibility checks readable and ensure every rejection is based on
  // explicit subtree facts.
  const bool hasInteractionScopedPasteSemantics =
      semantic.interactionSummary.hasPaste ||
      semantic.interactionSummary.hasStringifyPaste ||
      semantic.interactionSummary.hasWideStringifyPaste ||
      semantic.interactionSummary.hasRawInvocationPaste ||
      semantic.interactionSummary.hasChildSyntaxPaste;
  const bool hasStructuredSemantics = semantic.hasWrapperSemantics ||
                                      semantic.hasPreferredChildSyntax ||
                                      semantic.hasRawInvocationPreservation ||
                                      hasInteractionScopedPasteSemantics;
  const bool hasBridgeSensitiveStructuredSemantics =
      semantic.hasBridgeSensitiveStructuredSemantics;

  // Strict mode must fail closed when a paste-bearing subtree only
  // reaches its parent through passthrough flatten. That rewrite path
  // intentionally drops interior structural boundaries, which makes
  // nested pasted-token edits underdetermined: multiple replay candidates
  // can survive even though they share the same final pasted spelling.
  //
  // One narrow proof class is still admissible: if the accepted root
  // replay itself is a deferred wrapper-placeholder replay whose
  // rewritten syntax is known, and every rewritten root formal
  // corresponds to exactly one whole-child placeholder, then the parent
  // invocation is certified while only the child subtree is flattened. In
  // that case we are not inventing interior child structure; we are
  // preserving only the ancestor syntax that has already been proven
  // replayable.
  const bool allowRootPlaceholderFlattenReplay =
      semantic.hasAcceptedRootPlaceholderReplay &&
      semantic.rootReplayFlattensOnlyWholeChildArgs &&
      semantic.acceptedRootReplayInv && semantic.deferredPasteDischarge.valid &&
      !semantic.deferredPasteDischarge.deferredInvocations.empty() &&
      llvm::all_of(semantic.deferredPasteDischarge.deferredInvocations,
                   [&](const InvocationRewriteCertificate *invCert) {
                     return invCert && invCert->inv &&
                            invCert->inv->id ==
                                semantic.acceptedRootReplayInv->id;
                   });
  if (semantic.touchesPaste && semantic.hasPassthroughFlatten &&
      !allowRootPlaceholderFlattenReplay) {
    cert.valid = false;
    cert.failure =
        SubtreeSemanticAdmissibilityFailure::PasteWithPassthroughFlatten;
    cert.detail =
        "subtree semantic admissibility failed: paste-bearing subtree "
        "relies on passthrough flatten and therefore does not have a "
        "unique structure-preserving witness";
    return cert;
  }

  // Reaching this point means either paste+flatten did not occur or it
  // was justified by the explicit root-placeholder replay exception
  // computed above.  The remaining checks handle bridge-sensitive
  // structured semantics.

  // A lexical bridge can safely carry plain text, but it must not be the
  // remaining explanation for wrapper/raw-invocation/paste-sensitive
  // structure. If such semantics are still bridge-sensitive here, the
  // subtree has lost the structural witness needed for sound replay.
  if (hasBridgeSensitiveStructuredSemantics) {
    cert.valid = false;
    cert.failure = SubtreeSemanticAdmissibilityFailure::
        LexicalBridgeWithStructuredSemantics;
    cert.detail = "subtree semantic admissibility failed: bridge-sensitive "
                  "wrapper/raw-invocation/paste subtree semantics remain "
                  "inadmissible";
    return cert;
  }

  // All semantic hazards were either absent or explicitly discharged.
  // Record the summary facts used by the admissibility decision.
  cert.detail =
      formatv("subtree semantic admissibility: lexicalBridge={0} "
              "mixed={1} structuredSemantics={2} "
              "bridgeSensitiveStructuredSemantics={3} "
              "bridgedFormals={4} bridgedInteractions={5}",
              semantic.usesLexicalBridge ? 1 : 0,
              semantic.interactionSummary.hasMixedInteractions ? 1 : 0,
              hasStructuredSemantics ? 1 : 0,
              semantic.hasBridgeSensitiveStructuredSemantics ? 1 : 0,
              semantic.bridgedFormalKeys.size(),
              semantic.bridgedInteractionKeys.size())
          .str();
  return cert;
}

SubtreeSemanticCertificate
RefoldMacroDAGSubtreeCertifier::BuildSubtreeSemanticCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const InvocationRewriteCertificate &leafCert,
    ArrayRef<LiftChainCertificate> liftCertificates,
    ArrayRef<RootFormalMergeCertificate> rootMergeCertificates,
    const InvocationRewriteCertificate &rootCert) const {
  SubtreeSemanticCertificate cert;

  // Use a stable logical-formal key for bookkeeping across certificates.
  // The same invocation/formal can be reached through multiple lift paths.
  auto makeFormalKey = [&](const RefoldModel::MacroInvocation *inv,
                           uint32_t argIdx) -> std::string {
    return formatv("{0}#{1}", inv ? inv->id : 0, argIdx).str();
  };

  // Returns true iff the original root formal is exactly one top-level
  // child placeholder and nothing else. This is the structural predicate
  // for preserving the parent while flattening only that child.
  auto rootFormalIsWholeSingleChildPlaceholder =
      [&](const InvocationRewriteCertificate &invCert,
          uint32_t argIdx) -> bool {
    if (!invCert.inv)
      return false;
    auto rawArg =
        deps_.textPrimitives.GetInvocationArgText(*invCert.inv, argIdx);
    if (!rawArg)
      return false;
    auto placeholders = deps_.textPrimitives.GetTopLevelLexicalChildrenInArg(
        *invCert.inv, argIdx);
    return placeholders.size() == 1 && placeholders.front().child &&
           placeholders.front().relBegin == 0 &&
           placeholders.front().relEnd == rawArg->size();
  };

  // Slot certificates are where wrapper spelling decisions first become
  // visible. Fold them into the subtree feature bits used later by the
  // admissibility gate.
  auto recordSlotCertificate =
      [&](const SlotSemanticRewriteCertificate &slotCert) {
        cert.slotCertificates.push_back(slotCert);
        switch (slotCert.decision.kind) {
        case SlotRewriteDecisionKind::PreferredChildSyntax:
          cert.hasPreferredChildSyntax = true;
          break;
        case SlotRewriteDecisionKind::PreserveRawInvocation:
          cert.hasRawInvocationPreservation = true;
          break;
        case SlotRewriteDecisionKind::PassthroughFlatten:
          cert.hasPassthroughFlatten = true;
          break;
        }

        switch (slotCert.wrapperKind) {
        case WrapperChainKind::Exact:
          break;
        case WrapperChainKind::StringLiteral:
          cert.hasWrapperSemantics = true;
          cert.hasStringifySemantics = true;
          break;
        case WrapperChainKind::WideStringLiteral:
          cert.hasWrapperSemantics = true;
          cert.hasStringifySemantics = true;
          cert.hasWideStringifySemantics = true;
          break;
        }
      };

  // Argument certificates own the slot certificates for one rewritten
  // formal argument. Record both levels so later diagnostics can report
  // the complete proof trail.
  auto recordArgCertificate =
      [&](const ArgSemanticRewriteCertificate &argCert) {
        cert.argCertificates.push_back(argCert);
        for (const auto &slotCert : argCert.slotCertificates)
          recordSlotCertificate(slotCert);
      };

  // Record a formal rewrite certificate and all semantic evidence nested
  // below it. If this formal was produced by a lexical bridge, remember
  // that provenance and mark bridge-sensitive semantics when the formal
  // still depends on paste, stringify, raw invocation, child syntax, or
  // passthrough flattening.
  auto recordFormalCertificate = [&](const FormalRewriteCertificate &formalCert,
                                     bool bridgeSensitive) {
    cert.formalCertificates.push_back(formalCert);
    cert.formalInteractionConsistencies.push_back(
        formalCert.interactionConsistency);
    if (bridgeSensitive)
      cert.bridgedFormalKeys.insert(
          makeFormalKey(formalCert.inv, formalCert.argIdx));
    if (formalCert.validation.valid ||
        formalCert.validation.failure != RawFormalValidationFailure::None)
      cert.rawFormalValidations.push_back(formalCert.validation);
    for (const auto &interactionCert : formalCert.interactionCertificates) {
      cert.interactionCertificates.push_back(interactionCert);
      if (bridgeSensitive) {
        cert.bridgedInteractionKeys.insert(
            makeFormalKey(interactionCert.inv, interactionCert.argIdx));
        const auto &sig = formalCert.interactionConsistency.signature;
        if (sig.touchesPaste || sig.usesPreferredChildSyntax ||
            sig.usesRawInvocationPreservation || sig.usesPassthroughFlatten ||
            sig.usesStringify || sig.usesWideStringify ||
            sig.usesRawChildInvocationLogicalInput)
          cert.hasBridgeSensitiveStructuredSemantics = true;
      }
    }
    for (const auto &argCert : formalCert.argRewriteCertificates)
      recordArgCertificate(argCert);
  };

  // Invocation certificates contribute raw-formal validations and paste
  // validation state. Any required or failed paste validation makes the
  // subtree paste-sensitive for the final admissibility checks.
  auto recordInvocationCertificate =
      [&](const InvocationRewriteCertificate &invCert) {
        cert.invocationCertificates.push_back(invCert);
        for (const auto &validation : invCert.formalValidations)
          cert.rawFormalValidations.push_back(validation);
        if (invCert.pasteValidation.required ||
            !invCert.pasteValidation.valid) {
          cert.pasteValidations.push_back(invCert.pasteValidation);
          cert.touchesPaste = true;
        }
      };

  // Seed the bundle with the leaf certificate, then walk every lift chain
  // and record the proof artifacts produced at each structured hop.
  recordInvocationCertificate(leafCert);
  for (const auto &liftCert : liftCertificates) {
    cert.liftChains.push_back(liftCert);
    cert.usesLexicalBridge |= liftCert.usedLexicalBridge;
    for (const auto &step : liftCert.steps) {
      cert.structuredLiftCertificates.push_back(step);
      recordInvocationCertificate(step.currentCert);
      for (const auto &derivation : step.derivations)
        cert.parentDerivations.push_back(derivation);
      for (const auto &formalCert : step.parentFormalCertificates)
        recordFormalCertificate(
            formalCert, step.bridgedNextFormals.contains(formalCert.argIdx));
      if (step.parentCert.kind != InvocationRewriteCertificateKind::Invalid)
        recordInvocationCertificate(step.parentCert);
    }
  }

  for (const auto &mergeCert : rootMergeCertificates)
    cert.rootMergeCertificates.push_back(mergeCert);

  // The accepted root certificate is part of the same semantic bundle:
  // it may discharge deferred paste obligations or establish the narrow
  // root-placeholder flatten replay exception below.
  recordInvocationCertificate(rootCert);

  // Track whether the accepted root replay qualifies for the narrow
  // "preserve parent / flatten child" proof class. We only admit root
  // replays that are already uniquely certified deferred paste replays
  // with concrete rewritten syntax, and only when every rewritten formal
  // corresponds to one whole child placeholder in the original root.
  if (rootCert.kind == InvocationRewriteCertificateKind::Unique &&
      rootCert.pasteValidation.required && rootCert.pasteValidation.valid &&
      rootCert.pasteValidation.deferred &&
      !rootCert.rewrittenInvocationSyntax.empty() && rootCert.inv) {
    cert.hasAcceptedRootPlaceholderReplay = true;
    cert.acceptedRootReplayInv = rootCert.inv;
    cert.rootReplayFlattensOnlyWholeChildArgs = !rootCert.rewrites.empty();
    for (const auto &rewrite : rootCert.rewrites) {
      if (!rootFormalIsWholeSingleChildPlaceholder(rootCert, rewrite.argIdx)) {
        cert.rootReplayFlattensOnlyWholeChildArgs = false;
        break;
      }
    }
  }

  // Collapse the collected interaction certificates into subtree-wide
  // feature bits, then verify that repeated evidence for the same logical
  // formal agrees across all lift/root paths.
  cert.interactionSummary = this->BuildSubtreeInteractionSummaryCertificate(
      ctx, cert.interactionCertificates);
  cert.interactionConsistency =
      this->BuildSubtreeInteractionConsistencyCertificate(
          ctx, cert.formalCertificates);
  if (!cert.interactionConsistency.valid) {
    cert.valid = false;
    cert.detail = cert.interactionConsistency.detail;
    return cert;
  }

  // Deferred paste validations are permitted only if this complete bundle
  // contains an ancestor, root replay, or semantic witness that discharges
  // them.
  cert.deferredPasteDischarge =
      this->BuildSubtreeDeferredPasteDischargeCertificate(ctx, cert, rootCert);
  if (!cert.deferredPasteDischarge.valid) {
    cert.valid = false;
    cert.detail = cert.deferredPasteDischarge.detail;
    return cert;
  }

  // Final semantic gate: reject combinations that are individually proven
  // but not jointly admissible, such as bridge-sensitive structured
  // semantics surviving through a lexical bridge.
  cert.admissibility =
      this->BuildSubtreeSemanticAdmissibilityCertificate(ctx, cert);
  if (!cert.admissibility.valid) {
    cert.valid = false;
    cert.detail = cert.admissibility.detail;
    return cert;
  }

  cert.valid = true;
  cert.detail =
      formatv("subtree semantic bundle: invCerts={0} formalCerts={1} "
              "argCerts={2} slotCerts={3} interactions={4} "
              "formalConsistency={5} derivations={6} liftSteps={7} "
              "rootMerges={8} lexicalBridge={9} paste={10} "
              "wrappers={11} admissible={12} deferred={13} "
              "bridgedFormals={14} bridgedInteractions={15} "
              "bridgeSensitiveStructuredSemantics={16}",
              cert.invocationCertificates.size(),
              cert.formalCertificates.size(), cert.argCertificates.size(),
              cert.slotCertificates.size(), cert.interactionCertificates.size(),
              cert.formalInteractionConsistencies.size(),
              cert.parentDerivations.size(),
              cert.structuredLiftCertificates.size(),
              cert.rootMergeCertificates.size(), cert.usesLexicalBridge ? 1 : 0,
              cert.touchesPaste ? 1 : 0, cert.hasWrapperSemantics ? 1 : 0,
              cert.admissibility.valid ? 1 : 0,
              cert.deferredPasteDischarge.deferredInvocations.size(),
              cert.bridgedFormalKeys.size(), cert.bridgedInteractionKeys.size(),
              cert.hasBridgeSensitiveStructuredSemantics ? 1 : 0)
          .str();
  return cert;
}

SmallVector<DenseMap<uint32_t, FormalTextPair>, 4>
RefoldMacroDAGSubtreeCertifier::BuildLeafFormalLiftGroups(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &leaf,
    const DenseMap<uint32_t, FormalTextPair> &leafFormals) const {
  SmallVector<DenseMap<uint32_t, FormalTextPair>, 4> groups;
  if (leafFormals.empty())
    return groups;

  // Use a stable formal order so the resulting groups are deterministic
  // even though the input map is a DenseMap.
  SmallVector<uint32_t, 8> argOrder;
  argOrder.reserve(leafFormals.size());
  for (const auto &kvLocal : leafFormals)
    argOrder.push_back(kvLocal.first);
  llvm::sort(argOrder);

  DenseMap<uint32_t, SmallVector<uint32_t, 4>> adjacency;
  for (uint32_t argIdx : argOrder)
    adjacency[argIdx];

  // Group changed formals by the paste product they contribute to. The
  // key is the final pasted-token span, so all operands of the same paste
  // result land in the same bucket.
  StringMap<SmallVector<uint32_t, 4>> tokenArgs;
  for (const auto &ps : leaf.pasteSpans) {
    auto it = leafFormals.find(ps.argIdx);
    if (it == leafFormals.end())
      continue;

    std::string key = formatv("{0}:{1}", ps.begin, ps.end).str();
    auto &args = tokenArgs[key];
    if (llvm::find(args, ps.argIdx) == args.end())
      args.push_back(ps.argIdx);
  }

  // Add undirected edges between every pair of changed formals that share
  // a paste product. A connected component therefore represents the
  // smallest set of leaf formals that must be replayed together.
  for (const auto &kvLocal : tokenArgs) {
    ArrayRef<uint32_t> args = kvLocal.second;
    if (args.size() < 2)
      continue;
    for (size_t i = 0; i < args.size(); ++i) {
      for (size_t j = i + 1; j < args.size(); ++j) {
        if (llvm::find(adjacency[args[i]], args[j]) == adjacency[args[i]].end())
          adjacency[args[i]].push_back(args[j]);
        if (llvm::find(adjacency[args[j]], args[i]) == adjacency[args[j]].end())
          adjacency[args[j]].push_back(args[i]);
      }
    }
  }

  // Emit one formal map per connected component. Isolated changed formals
  // become singleton groups; paste-coupled formals become one shared
  // group so the later lift chain can preserve their joint semantics.
  DenseSet<uint32_t> visited;
  for (uint32_t rootArgIdx : argOrder) {
    if (!visited.insert(rootArgIdx).second)
      continue;

    SmallVector<uint32_t, 8> stack{rootArgIdx};
    DenseMap<uint32_t, FormalTextPair> groupFormals;
    while (!stack.empty()) {
      uint32_t argIdx = stack.pop_back_val();
      auto it = leafFormals.find(argIdx);
      if (it != leafFormals.end())
        groupFormals[argIdx] = it->second;

      auto adjIt = adjacency.find(argIdx);
      if (adjIt == adjacency.end())
        continue;
      for (uint32_t nextArgIdx : adjIt->second) {
        if (visited.insert(nextArgIdx).second)
          stack.push_back(nextArgIdx);
      }
    }

    if (!groupFormals.empty())
      groups.push_back(std::move(groupFormals));
  }

  return groups;
}

SubtreeRewriteCertificate
RefoldMacroDAGSubtreeCertifier::BuildSubtreeRewriteCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &leaf,
    const DenseMap<uint32_t, OldNewText> &leafEdits,
    bool deferLeafPasteValidation) const {
  SubtreeRewriteCertificate cert;
  cert.leaf = &leaf;
  cert.root = &ctx.rootInvocation;

  // Normalize the raw leaf edits into changed formal rewrites. No-change
  // edits are dropped here so later certificates only reason about actual
  // rewrite obligations.
  DenseMap<uint32_t, FormalTextPair> pendingLeafFormals;
  for (const auto &kvLocal : leafEdits) {
    StringRef oldText = StringRef(kvLocal.second.oldText).trim();
    StringRef newText = StringRef(kvLocal.second.newText).trim();
    if (oldText == newText)
      continue;
    pendingLeafFormals[kvLocal.first] =
        FormalTextPair{oldText.str(), newText.str()};
  }

  if (pendingLeafFormals.empty()) {
    cert.kind = SubtreeRewriteCertificateKind::NoChange;
    cert.detail = formatv("DAG subtree: leaf id={0} name={1} pending leaf "
                          "formals empty",
                          leaf.id, leaf.name)
                      .str();
    return cert;
  }

  // First prove that the edited leaf invocation can be rebuilt from the
  // changed leaf formals. This establishes the inner boundary before any
  // attempt is made to lift the rewrite outward through callers.
  cert.leafCert =
      deps_.structuredLifter.BuildWrapperPlaceholderHopInvocationCertificate(
          ctx, leaf, pendingLeafFormals, "DAG subtree leaf");
  if (cert.leafCert.kind == InvocationRewriteCertificateKind::Invalid) {
    cert.detail = cert.leafCert.detail;
    return cert;
  }
  if (cert.leafCert.kind == InvocationRewriteCertificateKind::NoChange) {
    cert.kind = SubtreeRewriteCertificateKind::NoChange;
    cert.detail = formatv("DAG subtree: leaf invocation no-change leaf "
                          "id={0} name={1} pendingLeafFormals={2} detail={3}",
                          leaf.id, leaf.name, pendingLeafFormals.size(),
                          cert.leafCert.detail)
                      .str();
    return cert;
  }

  // Use the leaf certificate's normalized rewrite list as the canonical
  // set of leaf formals to lift. This avoids carrying any raw input edits
  // that the leaf certificate did not accept.
  cert.leafFormals.clear();
  for (const auto &rewrite : cert.leafCert.rewrites) {
    cert.leafFormals[rewrite.argIdx] =
        FormalTextPair{rewrite.oldText, rewrite.newText};
  }

  DenseMap<uint32_t, SmallVector<FormalTextPair, 2>> rootRewrites;
  DenseSet<uint32_t> deferredRootOccurrenceArgIdxSet;

  // Paste-coupled leaf formals must be lifted together, while independent
  // formals can be lifted separately. Each lift group produces zero or
  // more candidate rewrites at the root invocation.
  auto liftGroups =
      this->BuildLeafFormalLiftGroups(ctx, leaf, cert.leafFormals);
  for (const auto &groupLeafFormals : liftGroups) {
    auto liftCert = deps_.structuredLifter.BuildLiftChainCertificate(
        ctx, leaf, groupLeafFormals);
    cert.liftCertificates.push_back(liftCert);
    if (liftCert.kind == LiftChainCertificateKind::Invalid) {
      cert.detail =
          !liftCert.detail.empty()
              ? liftCert.detail
              : formatv("DAG subtree: lift failed root id={0} "
                        "name={1} leaf id={2} name={3} "
                        "groupArgs={4}",
                        ctx.rootInvocation.id, ctx.rootInvocation.name, leaf.id,
                        leaf.name, formatUInt32List(liftCert.leafArgIdxs))
                    .str();
      return cert;
    }

    // Accumulate unique root-formal rewrites from all lift chains. The
    // actual compatibility check is delayed until the root merge
    // certificate so duplicate or overlapping chains are handled in one
    // proof location.
    for (const auto &rk : liftCert.rootFormals) {
      auto &rewrites = rootRewrites[rk.first];
      bool seen = false;
      for (const auto &existing : rewrites) {
        if (existing.oldText == rk.second.oldText &&
            existing.newText == rk.second.newText) {
          seen = true;
          break;
        }
      }
      if (!seen)
        rewrites.push_back(rk.second);
    }

    // Remember root arguments whose final text came through a lexical
    // bridge. Root replay may need to defer occurrence matching for these
    // arguments until the semantic bundle proves the bridge was consumed
    // exactly.
    for (uint32_t rootArgIdx : liftCert.bridgedRootArgIdxs)
      deferredRootOccurrenceArgIdxSet.insert(rootArgIdx);
  }

  if (rootRewrites.empty()) {
    cert.kind = SubtreeRewriteCertificateKind::NoChange;
    cert.detail = formatv("DAG subtree: no lifted root formals root id={0} "
                          "name={1} leaf id={2} name={3} leafCertRewrites={4} "
                          "liftCertificates={5}",
                          ctx.rootInvocation.id, ctx.rootInvocation.name,
                          leaf.id, leaf.name, cert.leafCert.rewrites.size(),
                          cert.liftCertificates.size())
                      .str();
    return cert;
  }

  // Merge all lifted rewrites per root formal against the original root
  // argument text. This is where independently lifted leaf groups are
  // required to agree before they are allowed to affect the root.
  DenseMap<uint32_t, FormalTextPair> pendingRootFormals;
  for (const auto &kvLocal : rootRewrites) {
    const uint32_t argIdx = kvLocal.first;
    auto mergeCert = deps_.structuredLifter.BuildRootFormalMergeCertificate(
        ctx, argIdx, kvLocal.second, "DAG subtree root merge");
    cert.rootMergeCertificates.push_back(mergeCert);
    if (mergeCert.kind == RootFormalMergeCertificateKind::Invalid) {
      cert.detail = mergeCert.detail;
      return cert;
    }
    if (mergeCert.kind == RootFormalMergeCertificateKind::NoChange) {
      pendingRootFormals[argIdx] =
          FormalTextPair{mergeCert.baseArgText, mergeCert.mergedArgText};
      continue;
    }

    pendingRootFormals[argIdx] =
        FormalTextPair{mergeCert.baseArgText, mergeCert.mergedArgText};
  }

  // Convert bridge provenance into the sorted root-argument list consumed
  // by root invocation certification.
  cert.deferRootOccurrenceArgIdxs.clear();
  cert.deferRootOccurrenceArgIdxs.reserve(pendingRootFormals.size());
  for (const auto &kvLocal : pendingRootFormals) {
    if (deferredRootOccurrenceArgIdxSet.contains(kvLocal.first))
      cert.deferRootOccurrenceArgIdxs.push_back(kvLocal.first);
  }
  llvm::sort(cert.deferRootOccurrenceArgIdxs);

  // Prove that the root invocation can be rewritten from the merged root
  // formals. Deferred occurrence arguments tell the root certificate
  // which bridged formal occurrences require semantic discharge later.
  cert.rootCert = deps_.structuredLifter.BuildInvocationRewriteCertificate(
      ctx, ctx.rootInvocation, pendingRootFormals, "DAG subtree root",
      ctx.subtreeValidationCtx.rootInvocationText,
      ctx.subtreeValidationCtx.rootInvocationArgRanges,
      cert.deferRootOccurrenceArgIdxs);
  if (cert.rootCert.kind == InvocationRewriteCertificateKind::Invalid) {
    if (cert.rootCert.failure == InvocationRewriteFailure::PasteMismatch) {
      // A plain root rewrite can fail on paste shape even when the root
      // still has a valid wrapper-placeholder replay. Probe that narrower
      // certificate before rejecting the whole subtree.
      auto wrapperProbe = deps_.structuredLifter
                              .BuildWrapperPlaceholderHopInvocationCertificate(
                                  ctx, ctx.rootInvocation, pendingRootFormals,
                                  "DAG subtree root probe");
      if (wrapperProbe.kind == InvocationRewriteCertificateKind::Unique) {
        cert.rootCert = std::move(wrapperProbe);
      }
    }
    if (cert.rootCert.kind == InvocationRewriteCertificateKind::Invalid) {
      cert.detail = cert.rootCert.detail;
      return cert;
    }
  }
  if (cert.rootCert.kind == InvocationRewriteCertificateKind::NoChange) {
    cert.kind = SubtreeRewriteCertificateKind::NoChange;
    cert.detail = formatv("DAG subtree: root invocation no-change root "
                          "id={0} name={1} pendingRootFormals={2} detail={3}",
                          ctx.rootInvocation.id, ctx.rootInvocation.name,
                          pendingRootFormals.size(), cert.rootCert.detail)
                      .str();
    return cert;
  }

  cert.rootFormals = pendingRootFormals;

  if (inTraceMode()) {
    // Emit a compact proof ledger before the semantic bundle is checked.
    // This makes it easier to distinguish lift/merge/root-cert failures
    // from later subtree semantic admissibility failures.
    SmallVector<uint32_t, 8> rootFormalArgIdxs;
    rootFormalArgIdxs.reserve(cert.rootFormals.size());
    for (const auto &kvLocal : cert.rootFormals)
      rootFormalArgIdxs.push_back(kvLocal.first);
    llvm::sort(rootFormalArgIdxs);
    trace("macro/proof",
          "DAG subtree proof ledger: root id={0} name={1} leaf id={2} "
          "name={3} rootFormals={4} deferredRootArgs={5} liftChains={6} "
          "leafPasteRequired={7} leafPasteDeferred={8} "
          "rootPasteRequired={9} "
          "rootPasteDeferred={10}",
          ctx.rootInvocation.id, ctx.rootInvocation.name, leaf.id, leaf.name,
          formatUInt32List(rootFormalArgIdxs),
          formatUInt32List(cert.deferRootOccurrenceArgIdxs),
          cert.liftCertificates.size(),
          cert.leafCert.pasteValidation.required ? 1 : 0,
          cert.leafCert.pasteValidation.deferred ? 1 : 0,
          cert.rootCert.pasteValidation.required ? 1 : 0,
          cert.rootCert.pasteValidation.deferred ? 1 : 0);
  }

  // The final semantic certificate checks global consistency conditions
  // that are not local to any single hop: repeated formal evidence,
  // deferred paste discharge, bridge-sensitive semantics, and admissible
  // paste/stringify/wrapper combinations.
  cert.semantic = this->BuildSubtreeSemanticCertificate(
      ctx, cert.leafCert, cert.liftCertificates, cert.rootMergeCertificates,
      cert.rootCert);
  if (!cert.semantic.valid) {
    cert.detail = cert.semantic.detail;
    return cert;
  }

  cert.kind = SubtreeRewriteCertificateKind::Unique;
  return cert;
}

UniformObservedLeafSeedCertificate
RefoldMacroDAGSubtreeCertifier::BuildUniformObservedLeafSeedCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
    ArrayRef<ObservedFormalConstraint> constraints,
    StringRef traceStage) const {
  UniformObservedLeafSeedCertificate cert;
  cert.inv = &inv;
  cert.argIdx = argIdx;

  if (constraints.empty()) {
    cert.failure = UniformObservedLeafSeedFailure::EmptyConstraints;
    cert.detail = formatv("{0}: uniform observed leaf seed unavailable: inv "
                          "id={1} name={2} argIdx={3} has no observed "
                          "constraints",
                          traceStage, inv.id, inv.name, argIdx)
                      .str();
    return cert;
  }

  // Uniformity is the proof condition: every observed constraint for this
  // leaf formal must normalize to exactly the same old/new rewrite.
  const StringRef oldTrim = StringRef(constraints[0].oldText).trim();
  const StringRef newTrim = StringRef(constraints[0].newText).trim();
  for (const auto &constraint : constraints) {
    if (StringRef(constraint.oldText).trim() != oldTrim ||
        StringRef(constraint.newText).trim() != newTrim) {
      cert.failure = UniformObservedLeafSeedFailure::DivergentConstraints;
      cert.detail = formatv("{0}: uniform observed leaf seed unavailable: "
                            "inv id={1} name={2} argIdx={3} observed "
                            "constraints diverged",
                            traceStage, inv.id, inv.name, argIdx)
                        .str();
      return cert;
    }
  }

  // The resulting seed is only the normalized leaf-side rewrite. Later
  // subtree/lift certificates must still prove how this reaches the root.
  cert.oldText = oldTrim.str();
  cert.newText = newTrim.str();
  if (oldTrim == newTrim) {
    cert.kind = UniformObservedLeafSeedCertificateKind::NoChange;
    cert.detail = formatv("{0}: uniform observed leaf seed collapsed to "
                          "no-change inv id={1} name={2} argIdx={3}",
                          traceStage, inv.id, inv.name, argIdx)
                      .str();
    return cert;
  }

  cert.kind = UniformObservedLeafSeedCertificateKind::Unique;
  cert.detail =
      formatv("{0}: uniform observed leaf seed certified inv id={1} "
              "name={2} argIdx={3} old='{4}' new='{5}'",
              traceStage, inv.id, inv.name, argIdx, cert.oldText, cert.newText)
          .str();
  return cert;
}

} // namespace refold
} // namespace clang
