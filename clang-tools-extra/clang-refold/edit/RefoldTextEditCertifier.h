//===--- RefoldTextEditCertifier.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Protected-source authority and materialization certification for TextEdits.
//
// Planners and emitters stamp two kinds of proven fact onto a TextEdit before
// final assembly: a capability to touch protected preprocessing structure, and
// the materialized B/output ranges and accepted-result carriers the edit
// realizes.  This service grants and records both, and owns the final global
// firewall that rechecks every capability against the emitted edit set.  It
// sits below planning, so candidate planners use it without depending on final
// text assembly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITCERTIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITCERTIFIER_H

#include "edit/RefoldEditTypes.h"
#include "model/RefoldToken.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "source/RefoldPreprocessingStructureKinds.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace clang {

class LangOptions;

namespace refold {

class RefoldModel;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndex;
class RefoldSourceMapper;
class RefoldTUAnchorProof;
class RefoldTerminalProofSink;
class RefoldTheoremAudit;

/// Return whether an authority is one of the two complete-source-closure
/// kinds.
///
/// These are the authorities minted only after a shared source-gap theorem
/// proved a complete indexed preprocessing interval as one source piece, which
/// is why they own `[begin,end)` exactly rather than a directive spelling.
/// The capability coverage rule here and the assembler's subsumption discharge
/// both ask this question, so they ask it in one place.
bool authorizationIsCompleteSourceClosure(
    ProtectedSourceEditAuthorityKind authority);

/// Return whether one complete directive interval ends with an unspliced
/// physical newline.
///
/// A final backslash-newline (or enabled trigraph equivalent) is a
/// continuation, not a logical-line terminator. Missing or out-of-bounds source
/// text therefore fails closed, and the exact active language mode participates
/// in the test.
bool preservedDirectiveHasTerminatingNewline(llvm::StringRef originalFileText,
                                             uint64_t sourceBegin,
                                             uint64_t sourceEnd,
                                             const LangOptions &lexLang);

/// Record \p authorization on \p edit unless an equal capability is already
/// recorded there.
void appendUniqueProtectedSourceAuthorization(
    TextEdit &edit, ProtectedSourceEditAuthorization authorization);

/// Grants protected-source capabilities, certifies materialized ranges and
/// accepted-result carriers on TextEdits, and audits the final edit set
/// against the capabilities it granted.
///
/// It does not choose candidates, order edits, or splice text: that remains
/// RefoldTextEditAssembler's.
class RefoldTextEditCertifier {
public:
  RefoldTextEditCertifier(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> bToks, const RefoldSourceMapper &sourceMapper,
      const RefoldPathIdentity &pathIdentity, const LangOptions &lexLang,
      const RefoldPreprocessingStructureIndex &tuPreprocessingStructureIndex,
      const RefoldTUAnchorProof &tuAnchorProof,
      const RefoldTerminalProofSink &terminalSink,
      const RefoldTheoremAudit &theoremAuditService);

  /// Declared out of line so the emission structure-index cache can hold a
  /// forward-declared RefoldPreprocessingStructureIndex.
  ~RefoldTextEditCertifier();

  /// \brief Attach accepted-result metadata to an emitted text edit.
  ///
  /// Copies the normalized accepted-result carrier selected by the proof
  /// lattice onto the concrete `TextEdit` that will be emitted. This preserves
  /// the accepted path, proof-discharge inventory, witnesses, and audit
  /// metadata at the byte-edit boundary so later validation/reporting can
  /// reason about the emitted edit without re-running candidate selection.
  void
  AttachAcceptedResultCarrier(TextEdit &edit,
                              const AcceptedResultCandidate &candidate) const;

  /// Authorize every complete protected interval touched by `[begin,end)` for
  /// one named specialized directive operation.
  ///
  /// The interval census is rebuilt in the exact physical source-owner domain
  /// that will later be assembled.  A narrow specialized operation must contain
  /// the scanner-proven directive spelling for every touched interval; a
  /// source-closure operation must contain the complete lexical interval.  The
  /// final capability is bound to exact physical kind/range identity. Producer
  /// metadata is carried and rechecked whenever the physical census can bind
  /// it, but a missing producer binding does not invalidate an already-proved
  /// physical operation.
  /// Failure returns false; callers must not emit the specialized edit without
  /// this capability.  Candidate planners may set `requestTerminalOnFailure`
  /// to false so their ordinary fallback lattice remains reachable.  The final
  /// emission audit always treats an authorization failure as terminal.
  bool AuthorizeProtectedSourceIntervals(
      TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
      llvm::StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef sourceBytes, uint64_t begin, uint64_t end,
      llvm::ArrayRef<PreprocessingStructureKind> allowedKinds,
      bool requireProtectedInterval = true,
      bool requestTerminalOnFailure = true) const;

  /// Authorize one exact protected interval already identified by a
  /// specialized semantic proof.
  ///
  /// Unlike `AuthorizeProtectedSourceIntervals`, this routine does not grant
  /// authority to every protected construct touched by the surrounding edit.
  /// It locates exactly one indexed interval matching the supplied physical
  /// transition range, verifies the named authority/kind pair, and records a
  /// capability for only that interval.  This is required by include-owned
  /// macro-state repair: the planner may move the one TU include that embodies
  /// a proved header `#define` or `#undef`, while any unrelated directive in
  /// the widened edit must remain unauthorized and therefore fail closed.
  /// `allowedNestedKinds` names the closed set of independently indexed
  /// constructs that are physically contained by that exact transition and
  /// semantically inseparable from it. The current macro-state theorem uses
  /// this only for `_Pragma` operators inside a complete macro replacement.
  bool AuthorizeExactProtectedSourceInterval(
      TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
      llvm::StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef sourceBytes, uint64_t intervalBegin,
      uint64_t intervalEnd,
      llvm::ArrayRef<PreprocessingStructureKind> allowedKinds,
      llvm::ArrayRef<PreprocessingStructureKind> allowedNestedKinds = {},
      bool requestTerminalOnFailure = true) const;

  /// Authorize a complete source-closure operation after its independent
  /// source-gap theorem has proved the entire physical byte envelope.
  ///
  /// Only the two named closure authorities are accepted.  The method expands
  /// their closed domain to every indexed preprocessing kind and records one
  /// exact capability per interval; it does not itself prove source closure.
  /// `preservedSourcePieces`, when engaged, is the caller's construct-by-
  /// construct statement of which crossed source ranges its replacement
  /// carries through as preserved source.  Supplying it lets this routine
  /// record every other authorized construct as eliminated, which is what a
  /// later subsumption theorem needs and what the closure authority alone
  /// cannot say.  Leaving it disengaged makes no claim and records nothing.
  bool AuthorizeCompleteProtectedSourceClosure(
      TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
      llvm::StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef sourceBytes, uint64_t begin, uint64_t end,
      bool requireProtectedInterval = true,
      bool requestTerminalOnFailure = true,
      std::optional<llvm::ArrayRef<std::pair<uint64_t, uint64_t>>>
          preservedSourcePieces = std::nullopt) const;

  /// Return whether an ordinary token-derived edit avoids every protected
  /// preprocessing interval in its exact physical source-owner domain.
  ///
  /// This is the candidate-level form of the final global firewall. It grants
  /// no capability and never interprets an accepted path name as directive
  /// authority. Candidate planners may keep their fallback lattice reachable
  /// by leaving `requestTerminalOnFailure` false.
  bool OrdinaryEditAvoidsProtectedPreprocessingStructure(
      const TextEdit &edit, llvm::StringRef sourcePath,
      std::optional<uint64_t> ownerIncludeId, llvm::StringRef sourceBytes,
      bool requestTerminalOnFailure = false) const;

  /// Certify a TextEdit with the B-byte range of the materialized surface.
  void CertifyTextEditMaterializedBByteRange(TextEdit &edit, uint64_t begin,
                                             uint64_t end) const;

  /// Certify that a TextEdit realizes no bytes of the edited preprocessed
  /// stream B.
  ///
  /// Use this only where the caller has proved that the emitted replacement is
  /// pure preprocessor state with no B-side payload. The certified edit is
  /// omitted from the materialized edit map instead of contributing an invented
  /// B-byte range; edits that simply reached emission without a certifier keep
  /// failing closed.
  void CertifyTextEditMaterializesNoBPayload(TextEdit &edit) const;

  /// Certify a TextEdit with the B-byte range described by a B-token envelope.
  void CertifyTextEditMaterializedBTokenRange(TextEdit &edit,
                                              uint64_t bTokBegin,
                                              uint64_t bTokEnd) const;

  /// Certify a TextEdit with the materialized ranges witnessed by a sideband
  /// edit proof. The complete sideband proof binds the emitted replacement
  /// payload to its raw-B byte provenance, so TU sideband emission should
  /// certify those two edit-map facts through this single gate rather than as
  /// independent fields.
  void CertifyTextEditMaterializedBReplayProof(
      TextEdit &edit, const SidebandPragmaEdit &sideband) const;

  /// Certify a TextEdit with the replacement-text subrange to report on the
  /// refolded-output side of the optional materialized edit map.
  void CertifyTextEditMaterializedOutputTextRange(TextEdit &edit,
                                                  uint64_t begin,
                                                  uint64_t end) const;

  /// Final global firewall for ordinary and specialized source edits.
  ///
  /// Path-specific planners remain the primary proof of token/source
  /// ownership.  This independent audit rechecks that direct-TU lexical
  /// widening contains only trivia plus exact capabilities and that no edit
  /// interferes with protected preprocessing structure without one exact,
  /// compatible specialized-operation authorization.
  bool AuditGlobalSourceEditInvariant(
      llvm::ArrayRef<TextEdit> edits, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner,
      std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef originalFileText) const;

private:
  /// Return the immutable protected-structure census for the physical source
  /// owner being assembled, reusing the run-wide TU index when possible.
  ///
  /// The census is a pure function of the physical source owner, the owner
  /// include occurrence, and the original file bytes, so the returned reference
  /// names a certifier-owned immutable index that stays valid for the
  /// certifier's lifetime.  Callers must not retain it beyond that.
  const RefoldPreprocessingStructureIndex &GetEmissionStructureIndex(
      llvm::StringRef emissionOwner,
      std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef originalFileText) const;

  const RefoldModel &model_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> bToks_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldPathIdentity &pathIdentity_;
  const LangOptions &lexLang_;
  const RefoldPreprocessingStructureIndex &tuPreprocessingStructureIndex_;
  const RefoldTUAnchorProof &tuAnchorProof_;
  const RefoldTerminalProofSink &terminalSink_;
  const RefoldTheoremAudit &theoremAuditService_;

  /// One built emission census plus the source extent it was built from.
  struct EmissionStructureIndexCacheEntry {
    std::unique_ptr<RefoldPreprocessingStructureIndex> index;
    /// Byte size of the original file text the index was built from.  This is
    /// the same discriminator the run-wide TU index reuse check applies, so a
    /// cache hit never substitutes a census built from a different extent.
    size_t sourceSize = 0;
  };

  /// Emission censuses keyed by `(physical source owner, owner include id)`.
  ///
  /// The census is immutable once built and is consulted by every per-edit and
  /// per-interval authorization predicate, so it is built at most once per
  /// source owner occurrence for the life of the certifier.
  mutable std::map<std::pair<std::string, std::optional<uint64_t>>,
                   EmissionStructureIndexCacheEntry>
      emissionStructureIndexCache_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITCERTIFIER_H
