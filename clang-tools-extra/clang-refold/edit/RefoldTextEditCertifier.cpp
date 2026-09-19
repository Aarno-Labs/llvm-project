//===--- RefoldTextEditCertifier.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Protected-source capability grants, the final global firewall that rechecks
// them, and materialized-range and accepted-result certification for
// TextEdits.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldTextEditCertifier.h"

#include "proof/RefoldSidebandReplayProof.h"

#include "edit/RefoldTUAnchorProof.h"
#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldTerminalProofSink.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldPreprocessingDirectiveScanner.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldSourceMapper.h"
#include "support/RefoldLog.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

bool authorizationIsCompleteSourceClosure(
    ProtectedSourceEditAuthorityKind authority) {
  return authority ==
             ProtectedSourceEditAuthorityKind::IncludePreservingSourceClosure ||
         authority == ProtectedSourceEditAuthorityKind::TUIncludeClosure;
}

bool preservedDirectiveHasTerminatingNewline(
    StringRef originalFileText, uint64_t sourceBegin, uint64_t sourceEnd,
    const LangOptions &lexLang) {
  if (sourceEnd <= sourceBegin || sourceEnd > originalFileText.size())
    return false;
  return sourceTextEndsWithNonSplicedPhysicalNewline(
      originalFileText.slice(sourceBegin, sourceEnd), lexLang);
}

void appendUniqueProtectedSourceAuthorization(
    TextEdit &edit, ProtectedSourceEditAuthorization authorization) {
  if (llvm::is_contained(edit.protectedSourceAuthorizations, authorization))
    return;
  edit.protectedSourceAuthorizations.push_back(std::move(authorization));
}

namespace {

/// Return whether one named exceptional operation is permitted to change the
/// indexed preprocessing construct.
///
/// This closed table is intentionally more restrictive than the
/// caller-supplied `allowedKinds`: the caller narrows the operation, while this
/// function prevents an accidental widening of the operation's theorem domain.
bool protectedSourceAuthorityAcceptsKind(
    ProtectedSourceEditAuthorityKind authority,
    PreprocessingStructureKind kind) {
  switch (authority) {
  case ProtectedSourceEditAuthorityKind::MacroStateRepair:
    return kind == PreprocessingStructureKind::MacroDefine ||
           kind == PreprocessingStructureKind::MacroUndef ||
           kind == PreprocessingStructureKind::PragmaOperator;
  case ProtectedSourceEditAuthorityKind::IncludeOwnedMacroStateRepair:
    return kind == PreprocessingStructureKind::Include ||
           kind == PreprocessingStructureKind::IncludeNext ||
           kind == PreprocessingStructureKind::Import;
  case ProtectedSourceEditAuthorityKind::SidebandPragmaEdit:
    return kind == PreprocessingStructureKind::Pragma ||
           kind == PreprocessingStructureKind::PragmaOperator;
  case ProtectedSourceEditAuthorityKind::IncludeMaterialization:
  case ProtectedSourceEditAuthorityKind::IncludeDirectiveRewrite:
    return kind == PreprocessingStructureKind::Include ||
           kind == PreprocessingStructureKind::IncludeNext ||
           kind == PreprocessingStructureKind::Import;
  case ProtectedSourceEditAuthorityKind::LineControlRepair:
    return kind == PreprocessingStructureKind::LineControl;
  case ProtectedSourceEditAuthorityKind::PragmaOnceGuardRewrite:
    // Deliberately excludes Import: `#import` establishes once-state with no
    // pragma at all, so the guard catalog does not model it and omitting it
    // here makes it fail closed at the emission firewall.
    //
    // PragmaOperator is admitted.  `_Pragma("once")` establishes exactly the
    // state the directive does, and the producer records it, so the only thing
    // that ever separated the two was whether the site's own bytes can carry
    // `#define <guard>`: an operator is an expression and may share its line,
    // where the replacement would not begin a logical line.  That is decided at
    // the site by `OnceSiteInPlaceReplacement`, which opens a line for the
    // directive when the operator does not begin a translated logical line and
    // declines the site outright when it cannot place one there at all -- and
    // it is decided again, independently, by
    // `operatorReplacementOwnsItsDirectiveLine` in the authorization below,
    // which is what keeps admitting the kind here from resting on the planner's
    // word.  Keeping the kind out of this table instead made every
    // operator-spelled header depend on the B-realized path, whose top-of-body
    // define is not equivalent for a conditional site -- and so refused those
    // headers entirely.
    return kind == PreprocessingStructureKind::Pragma ||
           kind == PreprocessingStructureKind::PragmaOperator ||
           kind == PreprocessingStructureKind::Include ||
           kind == PreprocessingStructureKind::IncludeNext;
  case ProtectedSourceEditAuthorityKind::IncludePreservingSourceClosure:
  case ProtectedSourceEditAuthorityKind::TUIncludeClosure:
    // These two paths own a separate source-gap/closure proof.  They may carry
    // complete directives of any indexed kind, but still only through exact
    // per-interval capabilities created after that proof succeeds.
    return true;
  case ProtectedSourceEditAuthorityKind::Unknown:
    return false;
  }
  return false;
}

ArrayRef<PreprocessingStructureKind> allProtectedStructureKinds() {
  static constexpr PreprocessingStructureKind kinds[] = {
      PreprocessingStructureKind::ConditionalIf,
      PreprocessingStructureKind::ConditionalIfdef,
      PreprocessingStructureKind::ConditionalIfndef,
      PreprocessingStructureKind::ConditionalElif,
      PreprocessingStructureKind::ConditionalElifdef,
      PreprocessingStructureKind::ConditionalElifndef,
      PreprocessingStructureKind::ConditionalElse,
      PreprocessingStructureKind::ConditionalEndif,
      PreprocessingStructureKind::MacroDefine,
      PreprocessingStructureKind::MacroUndef,
      PreprocessingStructureKind::Include,
      PreprocessingStructureKind::IncludeNext,
      PreprocessingStructureKind::Import,
      PreprocessingStructureKind::Pragma,
      PreprocessingStructureKind::PragmaOperator,
      PreprocessingStructureKind::LineControl,
      PreprocessingStructureKind::ErrorDirective,
      PreprocessingStructureKind::WarningDirective,
      PreprocessingStructureKind::OtherDirective};
  return kinds;
}

bool protectedSourceAuthorizationMatchesInterval(
    const ProtectedSourceEditAuthorization &authorization,
    const PreprocessingStructureInterval &interval) {
  return authorization.IsWellFormed() &&
         authorization.structureKind == interval.kind &&
         authorization.modelKind == interval.modelKind &&
         authorization.modelItemId == interval.modelItemId &&
         authorization.ownerConditionalArmId ==
             interval.ownerConditionalArmId &&
         authorization.conditionalGroupId == interval.conditionalGroupId &&
         authorization.conditionalArmId == interval.conditionalArmId &&
         authorization.begin == interval.begin &&
         authorization.end == interval.end;
}

/// Return the exact protected byte range that one named operation must own.
///
/// A complete source-closure operation must own the full indexed lexical
/// interval because its shared byte-cover theorem proved that complete source
/// piece, including logical-line trivia and the terminating newline. Narrow
/// specialized operations instead own the scanner-proven directive spelling.
/// These two authorities are intentionally noninterchangeable.
std::optional<std::pair<uint64_t, uint64_t>>
requiredProtectedCoverage(ProtectedSourceEditAuthorityKind authority,
                          const PreprocessingStructureInterval &interval) {
  if (authorizationIsCompleteSourceClosure(authority)) {
    // A source-closure authority is minted only after the shared source-gap
    // theorem proves the complete indexed preprocessing interval as one
    // source piece.  Requiring `[begin,end)` here preserves that theorem at
    // emission time, including leading logical-line trivia and the terminating
    // newline.  A narrower producer text range belongs only to a specialized
    // directive planner and must not weaken closure authority.
    if (!interval.IsValid())
      return std::nullopt;
    return std::make_pair(interval.begin, interval.end);
  }

  // A specialized directive planner already proved which physical operation
  // it is performing.  The final firewall therefore binds that proof to the
  // scanner's exact directive spelling, rather than attempting to reconstruct
  // producer authority from `text` fields that may legitimately differ from
  // the physical source (macro-computed include operands, comments in include
  // operands, and repeated zero-token header occurrences are all examples).
  // Requiring the complete scanner-proven spelling still rejects partial or
  // neighboring directive claims while avoiding a second, weaker producer
  // matching algorithm at the emission boundary.
  if (!interval.IsValid())
    return std::nullopt;
  return std::make_pair(interval.structureSpellingBegin,
                        interval.structureSpellingEnd);
}

/// Return whether an edit that writes a directive over `_Pragma` operator bytes
/// leaves that directive at the beginning of a translated logical line, and
/// ends the line it opens.
///
/// Every other protected kind this firewall admits occupies a directive line
/// already, so a replacement written over one inherits that position.  A pragma
/// operator does not: it is an expression, the bytes before it on its physical
/// line and the newline after it are ordinary source, and the physical line it
/// begins can itself be the continuation of the line above through an escaped
/// newline.  A `#` emitted over those bytes introduces a directive only if a
/// line was opened for it, and this is the emission boundary deciding that for
/// itself rather than trusting the planner that staged the edit.
///
/// The replacement grammar is closed, because the operation is: a planner
/// either deletes the operator or writes exactly one directive over it,
/// optionally opening a line before it and closing one after.  A replacement
/// outside that grammar -- more than one directive, a backslash that could
/// splice the following source into the directive, an enabled trigraph
/// introducer that could do the same -- is refused rather than analyzed.
bool operatorReplacementOwnsItsDirectiveLine(const TextEdit &edit,
                                             StringRef sourceBytes,
                                             const LangOptions &lexLang) {
  const StringRef text(edit.text);

  // Deleting the operator writes no directive at all.
  if (text.empty())
    return true;
  if (edit.start > sourceBytes.size() || edit.end > sourceBytes.size())
    return false;
  if (text.contains('\\') || (lexLang.Trigraphs && text.contains('?')))
    return false;

  const size_t hash = text.find('#');
  if (hash == StringRef::npos || text.find('#', hash + 1) != StringRef::npos)
    return false;

  // Opening a line is the only thing that may precede the directive.
  const StringRef head = text.take_front(hash);
  if (head.empty()) {
    if (edit.start != 0 &&
        !sourceTextEndsAtLogicalLineBeginning(sourceBytes.take_front(edit.start),
                                              lexLang))
      return false;
  } else if (head == "\n") {
    // The opened newline is a boundary only if the source it follows does not
    // end in an escaped newline that would consume it.
    if (!insertionBeginsWithNonSplicedPhysicalNewline(
            sourceBytes.take_front(edit.start), text, lexLang))
      return false;
  } else {
    return false;
  }

  // The directive's line must end before ordinary source resumes.  Either the
  // replacement closes it, in which case nothing may follow that newline in the
  // replacement, or the source the edit did not consume closes it with nothing
  // but horizontal white-space in between.
  const StringRef directive = text.drop_front(hash);
  const size_t newline = directive.find_first_of("\n\r");
  if (newline != StringRef::npos)
    return directive.drop_front(newline).ltrim("\r\n").empty();

  for (char byte : sourceBytes.drop_front(edit.end)) {
    if (byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f')
      continue;
    return byte == '\n' || byte == '\r';
  }
  return true;
}

/// Return whether `edit` owns every byte required by one exact capability.
bool protectedSourceAuthorizationCoversEdit(
    const ProtectedSourceEditAuthorization &authorization,
    const PreprocessingStructureInterval &interval, const TextEdit &edit) {
  if (!protectedSourceAuthorizationMatchesInterval(authorization, interval) ||
      !protectedSourceAuthorityAcceptsKind(authorization.authority,
                                           interval.kind))
    return false;
  std::optional<std::pair<uint64_t, uint64_t>> coverage =
      requiredProtectedCoverage(authorization.authority, interval);
  return coverage && edit.start <= coverage->first &&
         coverage->second <= edit.end;
}

bool sourceEditInterferesWithProtectedInterval(
    const TextEdit &edit, const PreprocessingStructureInterval &interval,
    StringRef sourceBytes, const LangOptions &lexLang) {
  if (edit.start < edit.end)
    return edit.start < interval.end && interval.begin < edit.end;

  // A pragma operator is one ordinary raw-token interval.  Its exact
  // half-open beginning and end are both normal token boundaries; token
  // adjacency and padding remain the responsibility of the candidate's
  // ordinary source-realization proof.
  if (interval.kind == PreprocessingStructureKind::PragmaOperator &&
      (edit.start == interval.begin || edit.start == interval.end))
    return false;

  // A zero-width insertion has no half-open overlap.  At the exact
  // beginning of a directive line it is nevertheless safe only when the
  // payload leaves the original directive introducer at the beginning of a
  // translated logical line: the payload must end in an unspliced physical
  // newline, optionally followed by horizontal white-space, which C permits
  // before `#`.  This is an exact lexical condition, not placement by
  // proximity.
  //
  // The horizontal-white-space tail is not a concession: token-aligned
  // diffing attributes the white-space run preceding the next surviving token
  // to the insertion hunk, so a payload that inserts whole lines before an
  // indented directive legitimately arrives as "...;\n    ".
  if (edit.start == interval.begin ||
      edit.start == interval.structureSpellingBegin) {
    if (edit.text.empty())
      return false;
    return !sourceTextEndsAtLogicalLineBeginning(edit.text, lexLang);
  }
  if (edit.start > interval.begin && edit.start < interval.end)
    return true;

  // A complete directive interval normally includes its terminating newline,
  // so insertion at `end` is then a distinct following source position.  At
  // EOF without a newline, insertion is safe only when its first byte terminates
  // the existing logical line before adding any new payload.
  if (edit.start != interval.end || interval.end != sourceBytes.size() ||
      interval.end == 0)
    return false;
  if (preservedDirectiveHasTerminatingNewline(
          sourceBytes, interval.begin, interval.end, lexLang) ||
      edit.text.empty())
    return false;
  return !insertionBeginsWithNonSplicedPhysicalNewline(
      sourceBytes.take_front(interval.end), edit.text, lexLang);
}

/// Return whether one accepted theorem carrier is compatible with a protected
/// source capability.
///
/// Exact interval identity is checked separately.  This closed table prevents
/// a capability from surviving normalization beside an unrelated carrier: in
/// particular, ordinary `TUByteSpan` results never support any protected-source
/// authority, even when an equivalent specialized edit was merged later.
bool acceptedResultSupportsProtectedSourceAuthority(
    const AcceptedResultCandidate &candidate,
    ProtectedSourceEditAuthorityKind authority) {
  const AcceptedPathKind path =
      candidate.proofSummary.inventory.currentPath;
  switch (authority) {
  case ProtectedSourceEditAuthorityKind::MacroStateRepair:
    return AcceptedResultIsSpecializedTUCarrier(
               candidate, AcceptedPathKind::TUByteSpanConservativeEdit) ||
           candidate.kind == AcceptedResultCandidateKind::MacroPatch ||
           (candidate.kind == AcceptedResultCandidateKind::IncludePatch &&
            path ==
                AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens);
  case ProtectedSourceEditAuthorityKind::IncludeOwnedMacroStateRepair:
    return AcceptedResultIsSpecializedTUCarrier(
        candidate, AcceptedPathKind::TUByteSpanConservativeEdit);
  case ProtectedSourceEditAuthorityKind::SidebandPragmaEdit:
    return AcceptedResultIsSpecializedTUCarrier(
               candidate, AcceptedPathKind::TUByteSpanConservativeEdit) ||
           (candidate.kind == AcceptedResultCandidateKind::IncludePatch &&
            (path == AcceptedPathKind::IncludeMaterializedExpansion ||
             path == AcceptedPathKind::IncludeRealizationInlineFromB));
  case ProtectedSourceEditAuthorityKind::LineControlRepair:
    return AcceptedResultIsSpecializedTUCarrier(
               candidate, AcceptedPathKind::TUByteSpanConservativeEdit) ||
           (candidate.kind == AcceptedResultCandidateKind::IncludePatch &&
            path ==
                AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens);
  case ProtectedSourceEditAuthorityKind::IncludeMaterialization:
    return candidate.kind == AcceptedResultCandidateKind::IncludePatch &&
           (path == AcceptedPathKind::IncludeMaterializedExpansion ||
            path == AcceptedPathKind::IncludeRealizationInlineFromB);
  case ProtectedSourceEditAuthorityKind::PragmaOnceGuardRewrite:
    // Synthetic once-state exists only because a header body was inlined, so the
    // only carriers that can support it are the two include-realization paths
    // that perform that inlining.
    return candidate.kind == AcceptedResultCandidateKind::IncludePatch &&
           (path == AcceptedPathKind::IncludeMaterializedExpansion ||
            path == AcceptedPathKind::IncludeRealizationInlineFromB);
  case ProtectedSourceEditAuthorityKind::IncludeDirectiveRewrite:
  case ProtectedSourceEditAuthorityKind::IncludePreservingSourceClosure:
    return candidate.kind == AcceptedResultCandidateKind::IncludePatch &&
           path == AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens;
  case ProtectedSourceEditAuthorityKind::TUIncludeClosure:
    return AcceptedResultIsSpecializedTUCarrier(candidate,
                                  AcceptedPathKind::TUIncludeClosureEdit);
  case ProtectedSourceEditAuthorityKind::Unknown:
    return false;
  }
  return false;
}

} // namespace

RefoldTextEditCertifier::RefoldTextEditCertifier(
    const RefoldModel &model, StringRef bSource, ArrayRef<PPTok> bToks,
    const RefoldSourceMapper &sourceMapper,
    const RefoldPathIdentity &pathIdentity, const LangOptions &lexLang,
    const RefoldPreprocessingStructureIndex &tuPreprocessingStructureIndex,
    const RefoldTUAnchorProof &tuAnchorProof,
    const RefoldTerminalProofSink &terminalSink,
    const RefoldTheoremAudit &theoremAuditService,
    ArrayRef<PrintedPragmaCarrier> printedPragmaCarriers)
    : model_(model), bSource_(bSource), bToks_(bToks),
      sourceMapper_(sourceMapper), pathIdentity_(pathIdentity),
      lexLang_(lexLang),
      tuPreprocessingStructureIndex_(tuPreprocessingStructureIndex),
      tuAnchorProof_(tuAnchorProof), terminalSink_(terminalSink),
      theoremAuditService_(theoremAuditService),
      printedPragmaCarriers_(printedPragmaCarriers) {}

RefoldTextEditCertifier::~RefoldTextEditCertifier() = default;

void RefoldTextEditCertifier::AttachAcceptedResultCarrier(
    TextEdit &edit, const AcceptedResultCandidate &candidate) const {
  theoremAuditService_.AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "AttachAcceptedResultCarrier");
  edit.acceptedResults.push_back(
      std::make_shared<AcceptedResultCandidate>(candidate));
}

const RefoldPreprocessingStructureIndex &
RefoldTextEditCertifier::GetEmissionStructureIndex(
    StringRef emissionOwner, std::optional<uint64_t> ownerIncludeId,
    StringRef originalFileText) const {
  if (pathIdentity_.PathsEqual(
          tuPreprocessingStructureIndex_.GetSourcePath(), emissionOwner) &&
      tuPreprocessingStructureIndex_.GetOwnerIncludeId() == ownerIncludeId &&
      tuPreprocessingStructureIndex_.GetSourceSize() ==
          originalFileText.size()) {
    return tuPreprocessingStructureIndex_;
  }

  // A cached census is reused only for the same source owner occurrence at the
  // same source extent, which is the exact discriminator the run-wide TU reuse
  // check above applies.  A different extent rebuilds and replaces the entry
  // rather than answering from a census of other bytes.
  EmissionStructureIndexCacheEntry &entry =
      emissionStructureIndexCache_[{emissionOwner.str(), ownerIncludeId}];
  if (entry.index && entry.sourceSize == originalFileText.size())
    return *entry.index;

  entry.index = std::make_unique<RefoldPreprocessingStructureIndex>(
      RefoldPreprocessingStructureIndex::Build(
          RefoldPreprocessingStructureIndex::Dependencies{
              model_, pathIdentity_, lexLang_},
          emissionOwner, originalFileText, ownerIncludeId));
  entry.sourceSize = originalFileText.size();
  return *entry.index;
}

bool RefoldTextEditCertifier::AuthorizeProtectedSourceIntervals(
    TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
    StringRef sourceBytes, uint64_t begin, uint64_t end,
    ArrayRef<PreprocessingStructureKind> allowedKinds,
    bool requireProtectedInterval, bool requestTerminalOnFailure) const {
  auto reject = [&](StringRef detail) {
    if (requestTerminalOnFailure) {
      theoremAuditService_.NoteTheoremAuditViolation(detail);
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/protected-source-authority", detail);
    }
    REFOLD_LOG_TRACE("edits/protected-source-authority", "{0}", detail);
    return false;
  };

  if (authority == ProtectedSourceEditAuthorityKind::Unknown || begin > end ||
      end > sourceBytes.size() || edit.start != begin || edit.end != end) {
    return reject("malformed protected-source authorization request");
  }

  const RefoldPreprocessingStructureIndex &structure = GetEmissionStructureIndex(
      sourcePath, ownerIncludeId, sourceBytes);
  if (!structure.IsProtectionCensusComplete())
    return reject("protected-source authorization has an incomplete physical "
                  "preprocessing census");

  bool authorizedAny = false;
  for (const PreprocessingStructureInterval &interval :
       structure.GetIntervals()) {
    if (!sourceEditInterferesWithProtectedInterval(edit, interval, sourceBytes,
                                                    lexLang_))
      continue;

    const bool requestedAuthorityAdmitsInterval =
        protectedSourceAuthorityAcceptsKind(authority, interval.kind) &&
        llvm::is_contained(allowedKinds, interval.kind);
    std::optional<std::pair<uint64_t, uint64_t>> requestedCoverage;
    if (requestedAuthorityAdmitsInterval)
      requestedCoverage = requiredProtectedCoverage(authority, interval);

    if (!requestedCoverage || begin > requestedCoverage->first ||
        requestedCoverage->second > end) {
      // A later specialized planner may widen an edit that already carries an
      // independent exact capability, for example macro-state repair around a
      // TU include-closure edit.  Preserve that composition only when the
      // existing capability both matches this indexed interval and owns the
      // complete byte range required by its own theorem.  A path label alone
      // can never authorize a partial producer spelling.
      const bool alreadyAuthorized = llvm::any_of(
          edit.protectedSourceAuthorizations,
          [&](const ProtectedSourceEditAuthorization &existing) {
            return protectedSourceAuthorizationCoversEdit(existing, interval,
                                                           edit);
          });
      if (alreadyAuthorized)
        continue;

      if (requestedAuthorityAdmitsInterval && requestedCoverage) {
        return reject(llvm::formatv(
                          "specialized edit [{0},{1}) does not contain the "
                          "required protected {2} coverage [{3},{4}) for "
                          "lexical interval [{5},{6})",
                          begin, end, toString(interval.kind),
                          requestedCoverage->first, requestedCoverage->second,
                          interval.begin, interval.end)
                          .str());
      }
      return reject(llvm::formatv(
                        "specialized edit authority does not admit protected "
                        "{0} interval [{1},{2})",
                        toString(interval.kind), interval.begin, interval.end)
                        .str());
    }

    ProtectedSourceEditAuthorization authorization;
    authorization.authority = authority;
    authorization.structureKind = interval.kind;
    authorization.modelKind = interval.modelKind;
    authorization.modelItemId = interval.modelItemId;
    authorization.ownerConditionalArmId = interval.ownerConditionalArmId;
    authorization.conditionalGroupId = interval.conditionalGroupId;
    authorization.conditionalArmId = interval.conditionalArmId;
    authorization.begin = interval.begin;
    authorization.end = interval.end;
    appendUniqueProtectedSourceAuthorization(edit, std::move(authorization));
    authorizedAny = true;
  }

  if (requireProtectedInterval && !authorizedAny)
    return reject("specialized directive operation did not match an exact "
                  "protected preprocessing interval");
  return true;
}

bool RefoldTextEditCertifier::AuthorizeExactProtectedSourceInterval(
    TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
    StringRef sourceBytes, uint64_t intervalBegin, uint64_t intervalEnd,
    ArrayRef<PreprocessingStructureKind> allowedKinds,
    ArrayRef<PreprocessingStructureKind> allowedNestedKinds,
    bool requestTerminalOnFailure) const {
  auto reject = [&](StringRef detail) {
    if (requestTerminalOnFailure) {
      theoremAuditService_.NoteTheoremAuditViolation(detail);
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/protected-source-authority", detail);
    }
    REFOLD_LOG_TRACE("edits/protected-source-authority", "{0}", detail);
    return false;
  };

  if (authority == ProtectedSourceEditAuthorityKind::Unknown ||
      intervalBegin >= intervalEnd || intervalEnd > sourceBytes.size() ||
      edit.start > intervalBegin || intervalEnd > edit.end) {
    return reject("malformed exact protected-source authorization request");
  }

  const RefoldPreprocessingStructureIndex &structure = GetEmissionStructureIndex(
      sourcePath, ownerIncludeId, sourceBytes);
  if (!structure.IsProtectionCensusComplete())
    return reject("exact protected-source authorization has an incomplete "
                  "physical preprocessing census");

  const PreprocessingStructureInterval *matched = nullptr;
  for (const PreprocessingStructureInterval &interval :
       structure.GetIntervals()) {
    // Specialized planners may identify one physical directive through any of
    // the structure index's three exact coordinate systems:
    //
    //  * `[begin,end)` is the complete lexical logical-line interval;
    //  * `[structureSpellingBegin,structureSpellingEnd)` is the scanner-proven
    //    directive spelling without leading trivia or the final newline; and
    //  * `[producerTextBegin,producerTextEnd)` is the exact source slice bound
    //    to the producer record.  Macro producer text commonly includes the
    //    terminating newline while omitting leading indentation, so it is not
    //    necessarily equal to either of the first two ranges.
    //
    // Accept only equality with one recorded range.  In particular, do not
    // infer authority from containment, adjacency, or textual similarity.
    const bool exactLexicalRange = interval.begin == intervalBegin &&
                                   interval.end == intervalEnd;
    const bool exactSpellingRange =
        interval.structureSpellingBegin == intervalBegin &&
        interval.structureSpellingEnd == intervalEnd;
    const bool exactProducerTextRange =
        interval.HasExactProducerTextRange() &&
        *interval.producerTextBegin == intervalBegin &&
        *interval.producerTextEnd == intervalEnd;
    if (!exactLexicalRange && !exactSpellingRange &&
        !exactProducerTextRange) {
      continue;
    }
    if (matched)
      return reject("exact protected-source authorization matched more than "
                    "one indexed preprocessing interval");
    matched = &interval;
  }

  if (!matched)
    return reject("exact protected-source authorization did not match an "
                  "indexed preprocessing interval");
  if (!llvm::is_contained(allowedKinds, matched->kind) ||
      !protectedSourceAuthorityAcceptsKind(authority, matched->kind)) {
    return reject(llvm::formatv(
                      "exact protected-source authority does not admit {0} "
                      "interval [{1},{2})",
                      toString(matched->kind), matched->begin, matched->end)
                      .str());
  }

  std::optional<std::pair<uint64_t, uint64_t>> requiredCoverage =
      requiredProtectedCoverage(authority, *matched);
  if (!requiredCoverage || edit.start > requiredCoverage->first ||
      requiredCoverage->second > edit.end) {
    return reject("exact protected-source authorization is not fully covered "
                  "by the emitted edit");
  }

  // The guard rewrite is the one authority that writes a directive over the
  // bytes of an expression, so it is the one that has to establish separately
  // that the directive it writes is a directive at all.  The other authorities
  // admitting a pragma operator carry their own replacement grammars and are
  // deliberately left to their own planners.
  if (authority == ProtectedSourceEditAuthorityKind::PragmaOnceGuardRewrite &&
      matched->kind == PreprocessingStructureKind::PragmaOperator &&
      !operatorReplacementOwnsItsDirectiveLine(edit, sourceBytes, lexLang_)) {
    return reject(llvm::formatv(
                      "once-guard rewrite of pragma operator [{0},{1}) emits a "
                      "directive that does not begin a translated logical line",
                      matched->structureSpellingBegin,
                      matched->structureSpellingEnd)
                      .str());
  }

  ProtectedSourceEditAuthorization authorization;
  authorization.authority = authority;
  authorization.structureKind = matched->kind;
  authorization.modelKind = matched->modelKind;
  authorization.modelItemId = matched->modelItemId;
  authorization.ownerConditionalArmId = matched->ownerConditionalArmId;
  authorization.conditionalGroupId = matched->conditionalGroupId;
  authorization.conditionalArmId = matched->conditionalArmId;
  authorization.begin = matched->begin;
  authorization.end = matched->end;
  SmallVector<ProtectedSourceEditAuthorization, 2> newAuthorizations;
  newAuthorizations.push_back(std::move(authorization));

  // A complete macro definition may contain a separately indexed `_Pragma`
  // operator in its replacement list. That operator is physically and
  // semantically part of the exact macro transition, but the final audit still
  // requires one capability per indexed interval. Authorize only explicitly
  // admitted nested kinds and reject any other protected construct enclosed by
  // the transition; containment alone never broadens the theorem domain.
  for (const PreprocessingStructureInterval &nested :
       structure.GetIntervals()) {
    if (&nested == matched || nested.begin < matched->begin ||
        matched->end < nested.end ||
        !sourceEditInterferesWithProtectedInterval(edit, nested, sourceBytes,
                                                    lexLang_)) {
      continue;
    }

    if (!llvm::is_contained(allowedNestedKinds, nested.kind) ||
        !protectedSourceAuthorityAcceptsKind(authority, nested.kind)) {
      return reject(llvm::formatv(
                        "exact protected-source transition contains "
                        "unadmitted nested {0} interval [{1},{2})",
                        toString(nested.kind), nested.begin, nested.end)
                        .str());
    }

    std::optional<std::pair<uint64_t, uint64_t>> nestedCoverage =
        requiredProtectedCoverage(authority, nested);
    if (!nestedCoverage || edit.start > nestedCoverage->first ||
        nestedCoverage->second > edit.end) {
      return reject("nested protected-source authorization is not fully "
                    "covered by the emitted edit");
    }

    ProtectedSourceEditAuthorization nestedAuthorization;
    nestedAuthorization.authority = authority;
    nestedAuthorization.structureKind = nested.kind;
    nestedAuthorization.modelKind = nested.modelKind;
    nestedAuthorization.modelItemId = nested.modelItemId;
    nestedAuthorization.ownerConditionalArmId = nested.ownerConditionalArmId;
    nestedAuthorization.conditionalGroupId = nested.conditionalGroupId;
    nestedAuthorization.conditionalArmId = nested.conditionalArmId;
    nestedAuthorization.begin = nested.begin;
    nestedAuthorization.end = nested.end;
    newAuthorizations.push_back(std::move(nestedAuthorization));
  }

  // Commit transactionally only after the exact transition and every admitted
  // nested interval have passed validation. A rejected specialized theorem
  // must not leave a partial capability on a caller-owned edit.
  for (ProtectedSourceEditAuthorization &newAuthorization :
       newAuthorizations) {
    appendUniqueProtectedSourceAuthorization(edit,
                                             std::move(newAuthorization));
  }
  return true;
}

bool RefoldTextEditCertifier::AuthorizeCompleteProtectedSourceClosure(
    TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
    StringRef sourceBytes, uint64_t begin, uint64_t end,
    bool requireProtectedInterval, bool requestTerminalOnFailure,
    std::optional<ArrayRef<std::pair<uint64_t, uint64_t>>>
        preservedSourcePieces) const {
  if (authority !=
          ProtectedSourceEditAuthorityKind::IncludePreservingSourceClosure &&
      authority != ProtectedSourceEditAuthorityKind::TUIncludeClosure) {
    if (requestTerminalOnFailure) {
      const StringRef detail =
          "complete source-closure authorization used a non-closure authority";
      theoremAuditService_.NoteTheoremAuditViolation(detail);
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/protected-source-authority", detail);
    }
    return false;
  }
  const size_t authorizationsBefore = edit.protectedSourceAuthorizations.size();
  if (!AuthorizeProtectedSourceIntervals(
          edit, authority, sourcePath, ownerIncludeId, sourceBytes, begin, end,
          allProtectedStructureKinds(), requireProtectedInterval,
          requestTerminalOnFailure))
    return false;

  // Only a caller that named its preserved pieces may have eliminations
  // recorded for it.  Silence is not evidence that nothing was preserved.
  if (!preservedSourcePieces)
    return true;


  for (size_t i = authorizationsBefore;
       i < edit.protectedSourceAuthorizations.size(); ++i) {
    const ProtectedSourceEditAuthorization &authorization =
        edit.protectedSourceAuthorizations[i];
    const bool preserved = llvm::any_of(
        *preservedSourcePieces, [&](const std::pair<uint64_t, uint64_t> &piece) {
          return authorization.begin < piece.second &&
                 piece.first < authorization.end;
        });
    if (preserved)
      continue;
    edit.eliminatedProtectedConstructs.push_back(
        EliminatedProtectedConstruct{authorization.structureKind,
                                     authorization.begin, authorization.end});
  }
  return true;
}

bool RefoldTextEditCertifier::OrdinaryEditAvoidsProtectedPreprocessingStructure(
    const TextEdit &edit, StringRef sourcePath,
    std::optional<uint64_t> ownerIncludeId, StringRef sourceBytes,
    bool requestTerminalOnFailure) const {
  auto reject = [&](StringRef detail) {
    if (requestTerminalOnFailure) {
      theoremAuditService_.NoteTheoremAuditViolation(detail);
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::EmissionEditSetComposable,
              TerminalFallbackFailureReason::UncomposableEmissionEditSet),
          "edits/ordinary-protected-source", detail);
    }
    REFOLD_LOG_TRACE("edits/ordinary-protected-source", "{0}", detail);
    return false;
  };

  if (edit.start > edit.end || edit.end > sourceBytes.size())
    return reject("ordinary source edit has an invalid physical byte range");

  const RefoldPreprocessingStructureIndex &structure = GetEmissionStructureIndex(
      sourcePath, ownerIncludeId, sourceBytes);
  if (!structure.IsProtectionCensusComplete())
    return reject("ordinary source edit has an incomplete physical "
                  "preprocessing census");

  for (const PreprocessingStructureInterval &interval :
       structure.GetIntervals()) {
    if (!sourceEditInterferesWithProtectedInterval(edit, interval,
                                                    sourceBytes, lexLang_))
      continue;
    return reject(llvm::formatv(
                      "ordinary edit [{0},{1}) interferes with protected {2} "
                      "interval [{3},{4})",
                      edit.start, edit.end, toString(interval.kind),
                      interval.begin, interval.end)
                      .str());
  }
  return true;
}

bool RefoldTextEditCertifier::AuditGlobalSourceEditInvariant(
    ArrayRef<TextEdit> edits, StringRef emissionStage,
    StringRef emissionOwner, std::optional<uint64_t> ownerIncludeId,
    StringRef originalFileText) const {
  auto reject = [&](StringRef detail) {
    theoremAuditService_.NoteTheoremAuditViolation(detail);
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::EmissionEditSetComposable,
            TerminalFallbackFailureReason::UncomposableEmissionEditSet),
        emissionStage, detail);
    REFOLD_LOG_TRACE("edits/global-source-audit", "{0}", detail);
    return false;
  };

  if (edits.empty())
    return true;
  if (emissionOwner.empty())
    return reject("global source-edit audit has no physical emission owner");

  const RefoldPreprocessingStructureIndex &structure = GetEmissionStructureIndex(
      emissionOwner, ownerIncludeId, originalFileText);
  if (!structure.IsProtectionCensusComplete())
    return reject("global source-edit audit has an incomplete physical "
                  "preprocessing census");

  // An edit that consumes the source printing a surviving `#pragma` line
  // removes that line from the output, so it must replay B's copy itself.  A
  // zero-width edit consumes the carrier only by landing strictly inside it.
  //
  // A recorded B range is a token envelope: it runs to the start of the next
  // token, past the last byte a replacement actually writes.  So the line
  // counts as replayed only when a B token inside the range follows it, which
  // puts it inside every realizer's replay, or when the range holds no token
  // at all and so replays sideband lines alone.
  const ArrayRef<size_t> bTokOff = sourceMapper_.BTokenByteOffsets();
  auto rangeReplaysLine = [&](uint64_t begin, uint64_t end,
                              const PrintedPragmaCarrier &carrier) {
    if (carrier.bLineBegin < begin || end < carrier.bLineEnd || bTokOff.empty())
      return false;
    const ArrayRef<size_t> tokens = bTokOff.drop_back();
    const size_t *firstInRange = llvm::lower_bound(tokens, begin);
    if (firstInRange == tokens.end() || *firstInRange >= end)
      return true;
    const size_t *firstAfterLine = llvm::lower_bound(tokens, carrier.bLineEnd);
    return firstAfterLine != tokens.end() && *firstAfterLine < end;
  };
  for (const PrintedPragmaCarrier &carrier : printedPragmaCarriers_) {
    if (carrier.ownerIncludeId != ownerIncludeId ||
        !pathIdentity_.PathsEqual(carrier.path, emissionOwner))
      continue;
    for (const TextEdit &edit : edits) {
      const bool consumes = edit.start == edit.end
                                ? carrier.sourceBegin < edit.start &&
                                      edit.start < carrier.sourceEnd
                                : edit.start < carrier.sourceEnd &&
                                      carrier.sourceBegin < edit.end;
      if (!consumes)
        continue;
      if (!edit.materializedBByteBegin || !edit.materializedBByteEnd ||
          !rangeReplaysLine(*edit.materializedBByteBegin,
                            *edit.materializedBByteEnd, carrier))
        return reject(
            llvm::formatv("edit source=[{0},{1}) consumes the source [{2},{3}) "
                          "printing a surviving #pragma line but does not "
                          "replay its B copy [{4},{5})",
                          edit.start, edit.end, carrier.sourceBegin,
                          carrier.sourceEnd, carrier.bLineBegin,
                          carrier.bLineEnd)
                .str());
    }
  }

  for (const TextEdit &edit : edits) {
    if (edit.start > edit.end || edit.end > originalFileText.size())
      return reject("global source-edit audit found an out-of-bounds edit");

    bool hasOrdinaryDirectTUCarrier = false;
    for (const std::shared_ptr<const AcceptedResultCandidate> &candidatePtr :
         edit.acceptedResults) {
      if (!candidatePtr || !AcceptedResultIsOrdinaryDirectTUCarrier(*candidatePtr))
        continue;
      hasOrdinaryDirectTUCarrier = true;

      // Audit the carrier's own proven source surface rather than only the
      // normalized TextEdit. Equivalent-edit merging may retain several
      // carriers with different original spans; no specialized capability on
      // the merged edit may retroactively authorize any one of them.
      const AcceptedResultCandidate &candidate = *candidatePtr;
      if (candidate.begin > candidate.end ||
          candidate.end > originalFileText.size() ||
          !candidate.hasPayloadPreview) {
        return reject("ordinary direct-TU carrier has malformed source "
                      "provenance");
      }
      TextEdit carrierSurface{candidate.begin,
                              candidate.end,
                              candidate.payloadPreview,
                              std::nullopt,
                              std::nullopt,
                              {},
                              {},
                              {}};
      for (const PreprocessingStructureInterval &interval :
           structure.GetIntervals()) {
        if (!sourceEditInterferesWithProtectedInterval(
                carrierSurface, interval, originalFileText, lexLang_))
          continue;
        if (inTraceMode()) {
          REFOLD_LOG_TRACE("tu/direct-span", "direct TU span rejected:");
          REFOLD_LOG_TRACE("tu/direct-span", "source=[{0},{1})",
                           candidate.begin, candidate.end);
          const bool macroState =
              interval.kind == PreprocessingStructureKind::MacroDefine ||
              interval.kind == PreprocessingStructureKind::MacroUndef;
          REFOLD_LOG_TRACE(
              "tu/direct-span", "reason={0}",
              macroState
                  ? "internal source gap contains protected macro state"
                  : "raw direct carrier contains protected preprocessing "
                    "structure");
          REFOLD_LOG_TRACE("tu/direct-span",
                           "protected structure=[{0},{1}) kind={2}",
                           interval.begin, interval.end, interval.kind);
          REFOLD_LOG_TRACE("tu/direct-span",
                           "required action=specialized structural repair or "
                           "terminal fallback");
        }
        return reject(llvm::formatv(
                          "ordinary direct-TU carrier [{0},{1}) interferes "
                          "with protected {2} interval [{3},{4})",
                          candidate.begin, candidate.end,
                          toString(interval.kind), interval.begin, interval.end)
                          .str());
      }
    }

    if (hasOrdinaryDirectTUCarrier &&
        !edit.protectedSourceAuthorizations.empty()) {
      return reject("ordinary direct-TU accepted carrier retained protected-"
                    "source authority after normalization");
    }
    if (edit.isDirectTUHunkEdit &&
        !edit.protectedSourceAuthorizations.empty()) {
      return reject("raw direct-TU edit provenance retained protected-source "
                    "authority");
    }

    for (const ProtectedSourceEditAuthorization &authorization :
         edit.protectedSourceAuthorizations) {
      const bool hasCompatibleCarrier = llvm::any_of(
          edit.acceptedResults,
          [&](const std::shared_ptr<const AcceptedResultCandidate> &candidate) {
            return candidate && acceptedResultSupportsProtectedSourceAuthority(
                                    *candidate, authorization.authority);
          });
      if (!hasCompatibleCarrier) {
        return reject(llvm::formatv(
                          "protected-source authority kind {0} has no "
                          "compatible specialized accepted carrier",
                          static_cast<unsigned>(authorization.authority))
                          .str());
      }
    }

    std::vector<bool> used(edit.protectedSourceAuthorizations.size(), false);
    for (const PreprocessingStructureInterval &interval :
         structure.GetIntervals()) {
      if (!sourceEditInterferesWithProtectedInterval(edit, interval,
                                                     originalFileText,
                                                     lexLang_))
        continue;

      bool matchedAuthorization = false;
      for (size_t i = 0; i < edit.protectedSourceAuthorizations.size(); ++i) {
        const ProtectedSourceEditAuthorization &authorization =
            edit.protectedSourceAuthorizations[i];
        if (!protectedSourceAuthorizationCoversEdit(authorization, interval,
                                                    edit))
          continue;
        // Equivalent duplicate edits may retain more than one independently
        // discharged authority for the same exact interval.  No selection is
        // required: all matching capabilities are consumed, and any capability
        // that does not match an actual interference is rejected below.
        used[i] = true;
        matchedAuthorization = true;
      }

      if (!matchedAuthorization) {
        return reject(llvm::formatv(
                          "ordinary edit [{0},{1}) overlaps protected {2} "
                          "interval [{3},{4}) without exact specialized "
                          "authority",
                          edit.start, edit.end, toString(interval.kind),
                          interval.begin, interval.end)
                          .str());
      }
    }

    for (size_t i = 0; i < edit.protectedSourceAuthorizations.size(); ++i) {
      const ProtectedSourceEditAuthorization &authorization =
          edit.protectedSourceAuthorizations[i];
      if (!authorization.IsWellFormed() || !used[i]) {
        return reject("emitted edit carries a malformed or unused protected-"
                      "source authorization");
      }
    }

    const bool hasAnyDirectTUProvenance =
        edit.directTUHunkIndex || edit.directTUHunkAStart ||
        edit.directTUHunkAEnd || edit.directTUHunkBStart ||
        edit.directTUHunkBEnd || edit.directTURawStart ||
        edit.directTURawEnd || edit.directTUFinalStart ||
        edit.directTUFinalEnd;
    if (!edit.isDirectTUHunkEdit && hasAnyDirectTUProvenance) {
      return reject("non-direct edit retained stale direct-TU provenance");
    }

    // Direct TU span planning proves the raw token-derived carrier. Recheck the
    // complete provenance tuple and later lexical widening independently at
    // final emission; missing fields are not treated as permission to skip the
    // firewall.
    if (edit.isDirectTUHunkEdit) {
      if (!edit.directTUHunkIndex || !edit.directTUHunkAStart ||
          !edit.directTUHunkAEnd || !edit.directTUHunkBStart ||
          !edit.directTUHunkBEnd || !edit.directTURawStart ||
          !edit.directTURawEnd || !edit.directTUFinalStart ||
          !edit.directTUFinalEnd) {
        return reject("direct TU edit carries incomplete raw/final provenance");
      }
      const uint64_t rawBegin = *edit.directTURawStart;
      const uint64_t rawEnd = *edit.directTURawEnd;
      const uint64_t finalBegin = *edit.directTUFinalStart;
      const uint64_t finalEnd = *edit.directTUFinalEnd;
      if (finalBegin != edit.start || finalEnd != edit.end ||
          finalEnd < finalBegin) {
        return reject("direct TU edit carries inconsistent raw/final span "
                      "provenance");
      }

      const bool ordinaryContainingSpan =
          finalBegin <= rawBegin && rawEnd <= finalEnd;
      const bool movedPureInsertion =
          edit.directTUHunkAStart && edit.directTUHunkAEnd &&
          edit.directTUHunkBStart && edit.directTUHunkBEnd &&
          *edit.directTUHunkAStart == *edit.directTUHunkAEnd &&
          *edit.directTUHunkBStart < *edit.directTUHunkBEnd &&
          rawBegin == rawEnd && finalBegin == finalEnd;
      const bool adjustedPureInsertion =
          movedPureInsertion && rawBegin != finalBegin;
      if (!ordinaryContainingSpan && !adjustedPureInsertion)
        return reject("direct TU edit does not contain its raw carrier and is "
                      "not a moved pure insertion");

      // Revalidate the original token-derived carrier against the immutable
      // source index instead of trusting that normalization preserved the
      // planner's earlier result. Provisional macro-transition evidence is
      // useful only before specialized repair; an ordinary direct carrier that
      // reaches final emission may not intersect any protected interval.
      if (!tuAnchorProof_.ValidateDirectTUEnvelope(emissionOwner, rawBegin,
                                             rawEnd)) {
        return reject("direct TU raw carrier failed final exact source-envelope "
                      "revalidation");
      }

      if (adjustedPureInsertion) {
        // Two movements of a direct insertion anchor are admitted, each across
        // structure that remains physically in place: in either direction
        // across printed pragmas, placing the insertion on the side of each
        // that B prints it; or forward past a source-authored line-control
        // prefix.  Recompute that exact topology here instead of trusting the
        // earlier adjustment witness after edit normalization.
        if (tuAnchorProof_.RangeHoldsOnlyPrintedPragmas(
                std::min(rawBegin, finalBegin), std::max(rawBegin, finalBegin)))
          continue;
        if (finalBegin < rawBegin)
          return reject("direct TU insertion moved backwards across structure "
                        "other than printed pragmas");
        uint64_t cursor = rawBegin;
        bool sawLineControl = false;
        for (const PreprocessingStructureInterval *interval :
             structure.FindOverlapping(rawBegin, finalBegin)) {
          if (interval->begin < rawBegin || finalBegin < interval->end ||
              interval->kind != PreprocessingStructureKind::LineControl ||
              interval->modelKind !=
                  PreprocessingStructureModelKind::LineControlEvent ||
              !interval->modelItemId || interval->begin < cursor ||
              !structure.IsRangeLexicallyIgnorable(cursor, interval->begin)) {
            return reject("direct TU insertion adjustment crosses structure "
                          "other than an exact producer-bound line-control "
                          "prefix");
          }
          sawLineControl = true;
          cursor = interval->end;
        }
        if (!sawLineControl ||
            !structure.IsRangeLexicallyIgnorable(cursor, finalBegin)) {
          return reject("direct TU insertion adjustment lacks a complete "
                        "source-line-control-plus-trivia proof");
        }
        continue;
      }

      TextEdit rawCarrier{rawBegin, rawEnd, edit.text, std::nullopt,
                          std::nullopt, {}, {}, {}};
      for (const PreprocessingStructureInterval &interval :
           structure.GetIntervals()) {
        if (!sourceEditInterferesWithProtectedInterval(
                rawCarrier, interval, originalFileText, lexLang_))
          continue;
        return reject("ordinary direct TU raw carrier intersects protected "
                      "preprocessing structure at final emission");
      }

      // Ordinary lexical widening has no exceptional capability. Every added
      // byte must therefore be exact lexer trivia; a specialized planner must
      // clear direct-hunk provenance and attach its own carrier before any
      // protected interval can be consumed.
      if (!structure.IsRangeLexicallyIgnorable(finalBegin, rawBegin) ||
          !structure.IsRangeLexicallyIgnorable(rawEnd, finalEnd)) {
        return reject("direct TU lexical widening contains non-trivia bytes");
      }
    }
  }

  return true;
}

void RefoldTextEditCertifier::CertifyTextEditMaterializedBByteRange(
    TextEdit &edit, uint64_t begin, uint64_t end) const {
  if (end < begin || end > static_cast<uint64_t>(bSource_.size()))
    REFOLD_LOG_FATAL("edit-map",
                     "invalid materialized B byte range [{0},{1}) bLen={2}",
                     begin, end, bSource_.size());
  if (edit.materializesNoBPayload)
    REFOLD_LOG_FATAL("edit-map",
                     "edit at source=[{0},{1}) was certified B-payload-free and "
                     "cannot also carry materialized B byte range [{2},{3})",
                     edit.start, edit.end, begin, end);
  edit.materializedBByteBegin = begin;
  edit.materializedBByteEnd = end;
}

void RefoldTextEditCertifier::CertifyTextEditMaterializesNoBPayload(
    TextEdit &edit) const {
  if (edit.materializedBByteBegin || edit.materializedBByteEnd)
    REFOLD_LOG_FATAL("edit-map",
                     "edit at source=[{0},{1}) cannot be both B-payload-free "
                     "and carry a materialized B byte range",
                     edit.start, edit.end);
  edit.materializesNoBPayload = true;
}

void RefoldTextEditCertifier::CertifyTextEditMaterializedBTokenRange(
    TextEdit &edit, uint64_t bTokBegin, uint64_t bTokEnd) const {
  std::optional<std::pair<uint64_t, uint64_t>> bytes =
      sourceMapper_.BTokenRangeToByteRange(bTokBegin, bTokEnd);
  if (!bytes)
    REFOLD_LOG_FATAL("edit-map",
                     "invalid materialized B token range [{0},{1}) bToks={2}",
                     bTokBegin, bTokEnd, bToks_.size());
  CertifyTextEditMaterializedBByteRange(edit, bytes->first, bytes->second);
}

void RefoldTextEditCertifier::CertifyTextEditMaterializedBReplayProof(
    TextEdit &edit, const SidebandPragmaEdit &sideband) const {
  const std::pair<uint64_t, uint64_t> bRange =
      sideband.MaterializedBByteRange();
  CertifyTextEditMaterializedBByteRange(edit, bRange.first, bRange.second);

  const std::pair<uint64_t, uint64_t> outputRange =
      sideband.MaterializedOutputTextRange();
  CertifyTextEditMaterializedOutputTextRange(edit, outputRange.first,
                                             outputRange.second);
}

void RefoldTextEditCertifier::CertifyTextEditMaterializedOutputTextRange(
    TextEdit &edit, uint64_t begin, uint64_t end) const {
  if (end < begin || end > static_cast<uint64_t>(edit.text.size()))
    REFOLD_LOG_FATAL(
        "edit-map",
        "invalid materialized output byte range [{0},{1}) textLen={2}", begin,
        end, edit.text.size());
  edit.materializedOutputTextBegin = begin;
  edit.materializedOutputTextEnd = end;
}

} // namespace refold
} // namespace clang
