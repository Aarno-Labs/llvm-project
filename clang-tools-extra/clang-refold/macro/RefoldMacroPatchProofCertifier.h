//===--- RefoldMacroPatchProofCertifier.h ----------------------*- C++ -*-===//
//
// Macro-patch proof certifier service for clang-refold.
//
// Owns the small, focused operations that *certify* a `MacroPatch` — i.e.,
// audit + write theorem-proof metadata (proof carriers, witnesses,
// accepted-result candidates, materialized-output ranges) onto the patch.
// The certifier is the shared proof-certification boundary for macro services
// that construct candidate patches but should not depend on planner-private
// methods.
//
// The certifier is intentionally a thin layer over the macro-patch proof
// classifier and the accepted-candidate builder: most methods either install a
// `MacroPatchProof` via `SetMacroPatchProof`, certify a whole-cover realization
// or a selected candidate through those services, or set materialized B-token
// / output-byte ranges directly on the patch.  No proof-summary construction
// happens here.
//
// Method signatures take only namespace-level types so the certifier never
// depends on caller-local context carriers; callers unwrap admission records
// into the raw data each certifier method requires.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPROOFCERTIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPROOFCERTIFIER_H

#include "edit/RefoldPatchTypes.h"
#include "model/RefoldModel.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldMacroPatchTypes.h"

#include <cstdint>

namespace clang {
namespace refold {

class RefoldAcceptedCandidateBuilder;
class RefoldMacroPatchProofClassifier;

/// Certifies macro-patch proof carriers.
///
/// The certifier attaches args-only, generated-callee, whole-cover, and
/// materialized-range proof facts to MacroPatch objects before they reach
/// selection or emission.
class RefoldMacroPatchProofCertifier {
public:
  /// Borrowed inputs needed to certify macro-patch proofs.
  struct Dependencies {
    /// Installs proof carriers and certifies whole-cover realizations.
    const RefoldMacroPatchProofClassifier &macroPatchProofClassifier;
    /// Builds the accepted-result carrier a certified patch selects.
    const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder;
  };

  explicit RefoldMacroPatchProofCertifier(Dependencies deps);

  /// Record the materialized output byte range carried by an invocation
  /// rewrite directly on the patch's materialization record.
  void
  CertifyInvocationRewriteMaterializedOutputRange(MacroPatch &patch,
                                                  uint64_t outputByteStart,
                                                  uint64_t outputByteEnd) const;

  /// Certify the macro candidate that the final selector chose: if the
  /// selected candidate carries an emitted accepted candidate, attach it to
  /// the patch through the accepted-candidate builder; otherwise leave the
  /// patch uncertified and trace the selector-only outcome.
  void CertifySelectedFinalMacroCandidate(
      const RefoldModel::MacroInvocation &invocation,
      const SelectedMacroSelectionCandidate &selectedCandidate,
      MacroPatch &selectedPatch) const;

  /// Certify a whole-cover accepted candidate.  Whole-cover realization has
  /// exactly one certifying operation, so this is the single entry point for
  /// it: it installs the realization proof carrier and records the B-payload
  /// partition together.
  void CertifyWholeCoverAcceptedCandidate(
      MacroPatch &patch, const WholeCoverPlan &plan,
      const RefoldModel::MacroInvocation &invocation) const;

  /// Build and install the standard args-only macro proof on the patch,
  /// optionally enriched with a whole-envelope replay witness.
  void
  SetArgsOnlyStandardProof(MacroPatch &patch,
                           const RefoldModel::MacroInvocation &m,
                           bool wholeEnvelopeReplayValidated,
                           bool definitionTapeReplayValidated = false) const;

  /// Enrich an already-validated whole-envelope replay proof with a
  /// generated-callee replay witness.  Does not reclassify the proof.
  void CertifyGeneratedCalleeReplayProof(
      MacroPatch &patch, const RefoldModel::MacroInvocation &m,
      uint64_t finalDirectiveId, uint32_t generatedCallDepth,
      uint32_t objectAliasHops, bool usesStringification, bool usesPaste,
      bool usesVariadicForwarding, bool decodedStringLiteralEvidenceOnly) const;

  /// Install the recursive tuple-generated-callee proof carrier.
  ///
  /// The caller must have already proven the recursive theorem obligations and
  /// populated the witness with durable producer-path and tuple-slice facts.
  /// This method only attaches the first-class proof identity and rebuilds the
  /// normalized proof summary through the macro-patch proof classifier.
  void SetRecursiveTupleGeneratedCalleeReplayProof(
      MacroPatch &patch, const RefoldModel::MacroInvocation &rootInvocation,
      RecursiveTupleGeneratedCalleeReplayWitness witness) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPROOFCERTIFIER_H
