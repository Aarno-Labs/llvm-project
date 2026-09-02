//===--- RefoldMacroDAGStructuredLifter.cpp ---------------------*- C++ -*-===//
//
// Structured invocation-lift and root-formal merge service for macro DAGs.
//
// This translation unit builds invocation rewrite certificates, parent
// constraint certificates, structured lift chains, and root-formal merge
// certificates from a caller-provided DAG lifting context.  It consumes text
// primitives and invertibility certificates through Dependencies and returns
// explicit certificate values rather than mutating planner-global proof state.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDAGStructuredLifter.h"

#include "core/RefoldLog.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroTopology.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroDAGStructuredLifter::RefoldMacroDAGStructuredLifter(
    Dependencies deps)
    : deps_(std::move(deps)) {}

namespace {

/// Return whether \p values already holds the exact spelling \p candidate.
///
/// Probing through StringRef keeps the comparison byte-for-byte identical to a
/// std::string comparison while avoiding a temporary std::string per probe.
static bool containsSpelling(ArrayRef<std::string> values,
                             StringRef candidate) {
  return llvm::any_of(values, [&](const std::string &value) {
    return StringRef(value) == candidate;
  });
}

/// Resolves the unique inverse split of a rewritten pasted-token core.
///
/// The resolver is deliberately local to this translation unit because it owns
/// no proof state and trusts callers to supply already-validated paste operands,
/// delimiters, and sibling-surface context. Its only mutable state is the DFS
/// frontier and the distinct split solutions needed to preserve the existing
/// fail-closed ambiguity cutoff.
class PastedCoreSplitResolver {
public:
  PastedCoreSplitResolver(ArrayRef<StringRef> oldSegs,
                          ArrayRef<StringRef> midBodies)
      : oldSegs_(oldSegs), midBodies_(midBodies) {}

  /// Splits a rewritten pasted-token core into one segment per original paste
  /// operand.
  ///
  /// The trusted inputs are the already-rebased old operand surfaces and the
  /// non-empty delimiter text that originally separated adjacent paste
  /// operands. This resolver owns only the inverse-split search obligation:
  /// it tries delimiter occurrences in left-to-right order, reserves delimiter
  /// occurrences that must remain inside neighboring operands, and records
  /// distinct segmentations in the same order the local DFS used before this
  /// extraction. If more than one distinct segmentation is found, the replay is
  /// ambiguous and the caller must fail closed rather than choose a split
  /// heuristically. Sibling-surface contradiction checks and proof admission
  /// remain with the callers that consume the unique split.
  std::optional<SmallVector<StringRef, 4>> Resolve(StringRef core) {
    Dfs(0, core);
    if (splitSolutions_.size() != 1)
      return std::nullopt;
    return std::move(splitSolutions_[0]);
  }

private:
  uint64_t SuffixDelimiterNeed(size_t delimIdx) const {
    const StringRef delim = midBodies_[delimIdx];
    uint64_t need = 0;
    for (size_t segIdx = delimIdx + 1; segIdx < oldSegs_.size(); ++segIdx)
      need += countSubstringOccurrences(oldSegs_[segIdx], delim);
    for (size_t later = delimIdx + 1; later < midBodies_.size(); ++later)
      if (midBodies_[later] == delim)
        ++need;
    return need;
  }

  void AddSplitSolution(const SmallVectorImpl<StringRef> &parts) {
    SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
    for (const auto &existing : splitSolutions_)
      if (existing == copy)
        return;
    splitSolutions_.push_back(std::move(copy));
  }

  void Dfs(size_t delimIdx, StringRef rest) {
    if (splitSolutions_.size() > 1)
      return;
    if (delimIdx == midBodies_.size()) {
      curSegs_.push_back(rest);
      AddSplitSolution(curSegs_);
      curSegs_.pop_back();
      return;
    }

    const StringRef delim = midBodies_[delimIdx];
    const uint64_t needLeft =
        countSubstringOccurrences(oldSegs_[delimIdx], delim);
    const uint64_t needRight = SuffixDelimiterNeed(delimIdx);

    // Try each occurrence of the old delimiter as the next split point. The
    // occurrence-count guards keep delimiter text that originally belonged
    // inside neighboring segments from being consumed as a split.
    for (size_t pos = 0; (pos = rest.find(delim, pos)) != StringRef::npos;
         ++pos) {
      StringRef left = rest.slice(0, pos);
      StringRef tail = rest.drop_front(pos + delim.size());
      if (countSubstringOccurrences(left, delim) < needLeft)
        continue;
      if (countSubstringOccurrences(tail, delim) < needRight)
        continue;
      curSegs_.push_back(left);
      Dfs(delimIdx + 1, tail);
      curSegs_.pop_back();
    }
  }

  ArrayRef<StringRef> oldSegs_;
  ArrayRef<StringRef> midBodies_;
  SmallVector<StringRef, 4> curSegs_;
  SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions_;
};

/// Returns the unique delimiter-respecting pasted-core split, or std::nullopt
/// when the original delimiter ordering admits no split or more than one
/// distinct split. Callers preserve proof construction and sibling-surface
/// checks by validating the returned segment count before consuming it.
std::optional<SmallVector<StringRef, 4>> splitPastedCoreByDelimiters(
    StringRef core, ArrayRef<StringRef> oldSegs, ArrayRef<StringRef> midBodies) {
  PastedCoreSplitResolver resolver(oldSegs, midBodies);
  return resolver.Resolve(core);
}

/// Rebuilds exact original nested paste syntax from already-certified DAG
/// evidence.
///
/// This resolver owns the recursive preservation search for
/// `BuildStructuredLiftCertificate`. Its trusted
/// inputs are the caller's DAG lifting context, the topology/text/proof
/// services borrowed through Dependencies, and a direct-paste derivation
/// callback that has already preserved delimiter splitting, sibling-surface
/// checks, and parent-constraint proof construction. The resolver preserves
/// child traversal order from `MacroChildrenOf`, keeps derived formal
/// constraints in their existing sorted order, and fails closed whenever
/// direct-child selection is ambiguous or a nested child/formal certificate
/// cannot uniquely rebuild the original paste shape. It does not rank
/// candidates or admit edits; it only returns replay syntax after the existing
/// wrapper-placeholder proof gate accepts the reconstructed invocation.
class ExactOriginalShapePasteReplayResolver {
public:
  using DerivedConstraintList =
      SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>;
  using DirectPasteDerivationFn = function_ref<std::optional<DerivedConstraintList>(
      const RefoldModel::MacroInvocation &, const RefoldModel::MacroInvocation &,
      StringRef, StringRef, StringRef)>;

  ExactOriginalShapePasteReplayResolver(
      const RefoldMacroDAGStructuredLifter &lifter,
      const RefoldMacroDAGStructuredLifter::Dependencies &deps,
      const RefoldMacroDAGLiftingContext &ctx,
      DirectPasteDerivationFn deriveDirectPasteConstraints)
      : lifter_(lifter), deps_(deps), ctx_(ctx),
        deriveDirectPasteConstraints_(deriveDirectPasteConstraints) {}

  /// Returns exact nested replay syntax for `target` when the observed rewrite
  /// still has one certificate-backed direct-paste explanation.
  ///
  /// The observed surfaces are trusted to be the hop-local old/new texts being
  /// discharged by the caller. This method preserves the previous recursive
  /// ordering: scan direct children in topology order, prefer a sole full-arg
  /// nested child before falling back to the normal formal certificate, then
  /// rebuild the target invocation through the existing wrapper-placeholder
  /// certificate. A second child with a different derivation, a failed nested
  /// proof, or any invalid formal/invocation certificate rejects the replay
  /// rather than choosing a heuristic paste shape.
  std::optional<std::string> Resolve(
      const RefoldModel::MacroInvocation &target, StringRef observedOld0,
      StringRef observedNew0, StringRef traceStage) const {
    StringRef observedOld = observedOld0.trim();
    StringRef observedNew = observedNew0.trim();
    if (!target.invText)
      return std::nullopt;

    StringRef rawTarget = StringRef(*target.invText).trim();
    if (rawTarget.empty())
      return std::nullopt;
    if (observedOld == observedNew)
      return rawTarget.str();

    ArrayRef<const RefoldModel::MacroInvocation *> children =
        deps_.macroTopology.MacroChildrenOf(target.id);
    if (children.empty())
      return std::nullopt;

    const RefoldModel::MacroInvocation *directPasteChild = nullptr;
    std::optional<DerivedConstraintList> derivedConstraints;
    if (!TrySelectDirectChildForSurface(target, children, observedOld,
                                        observedNew, traceStage,
                                        directPasteChild, derivedConstraints))
      return std::nullopt;
    if (!directPasteChild) {
      // Some observed paste surfaces are represented through the quoted
      // spelling produced by stringification. Retry with quoted surfaces, but
      // still require the same unique direct-child proof.
      std::string quotedOld = stringutils::quoteCStringLiteral(observedOld);
      std::string quotedNew = stringutils::quoteCStringLiteral(observedNew);
      if (!TrySelectDirectChildForSurface(target, children, quotedOld, quotedNew,
                                          traceStage, directPasteChild,
                                          derivedConstraints))
        return std::nullopt;
    }
    if (!directPasteChild || !derivedConstraints)
      return std::nullopt;

    // Group the derived constraints by the target's formals. Each group is
    // later replayed either by recursively preserving a nested child or by
    // falling back to the normal observed-formal certificate.
    DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>> groupedObserved;
    for (const auto &kv : *derivedConstraints)
      groupedObserved[kv.first].push_back(kv.second);

    DenseMap<uint32_t, FormalTextPair> targetFormals;
    for (const auto &kvLocal : groupedObserved) {
      const uint32_t formalIdx = kvLocal.first;
      auto argText =
          deps_.textPrimitives.GetInvocationArgText(target, formalIdx);
      if (!argText)
        return std::nullopt;
      const StringRef rawOldArg = argText->trim();

      // When the target formal is exactly one nested child invocation, try to
      // preserve that nested child first. This keeps a chain such as
      // JOIN(JOIN(...), ...) instead of collapsing it to the already
      // materialized pasted token.
      if (kvLocal.second.size() == 1) {
        const StringRef segOld =
            StringRef(kvLocal.second.front().oldText).trim();
        const StringRef segNew =
            StringRef(kvLocal.second.front().newText).trim();
        auto placeholders = deps_.textPrimitives.GetTopLevelLexicalChildrenInArg(
            target, formalIdx);
        if (placeholders.size() == 1 && placeholders.front().child &&
            placeholders.front().relBegin == 0 &&
            placeholders.front().relEnd == rawOldArg.size()) {
          const RefoldModel::MacroInvocation *nestedChild =
              placeholders.front().child;
          if (auto nestedSyntax = Resolve(
                  *nestedChild, segOld, segNew,
                  "DAG per-hop exact original-shape replay")) {
            targetFormals[formalIdx] =
                FormalTextPair{rawOldArg.str(), std::move(*nestedSyntax)};
            continue;
          }
        }
      }

      // Otherwise, certify the formal rewrite in the usual way and let the
      // wrapper-hop certificate rebuild the target invocation around it.
      auto formalCert =
          deps_.invertibilitySolver.BuildObservedFormalRewriteCertificate(
              target, formalIdx, kvLocal.second,
              /*preferredChildSyntax=*/nullptr, traceStage, ctx_.tokenHunksAR);
      if (formalCert.kind == FormalRewriteCertificateKind::Invalid)
        return std::nullopt;
      targetFormals[formalIdx] =
          FormalTextPair{formalCert.oldText, formalCert.newText};
    }

    // Finally, prove that the rewritten formals still fit the target's
    // original placeholder structure. This is the soundness gate for the
    // exact-shape replay at this invocation boundary.
    auto replayCert = lifter_.BuildWrapperPlaceholderHopInvocationCertificate(
        ctx_, target, targetFormals, traceStage);
    if (replayCert.kind == InvocationRewriteCertificateKind::Invalid)
      return std::nullopt;
    if (!replayCert.rewrittenInvocationSyntax.empty())
      return replayCert.rewrittenInvocationSyntax;

    // If the placeholder certificate did not directly materialize syntax,
    // build it from only the formals that actually changed.
    DenseMap<uint32_t, std::string> replByFormal;
    for (const auto &kvLocal : targetFormals) {
      StringRef oldText = StringRef(kvLocal.second.oldText).trim();
      StringRef newText = StringRef(kvLocal.second.newText).trim();
      if (oldText != newText)
        replByFormal[kvLocal.first] = newText.str();
    }
    return deps_.textPrimitives.BuildRewrittenInvocationSyntax(target,
                                                               replByFormal);
  }

private:
  static bool SameDerivedConstraints(const DerivedConstraintList &lhs,
                                     const DerivedConstraintList &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
      if (lhs[i].first != rhs[i].first)
        return false;
      if (lhs[i].second.oldText != rhs[i].second.oldText ||
          lhs[i].second.newText != rhs[i].second.newText)
        return false;
    }
    return true;
  }

  bool TrySelectDirectChildForSurface(
      const RefoldModel::MacroInvocation &target,
      ArrayRef<const RefoldModel::MacroInvocation *> children,
      StringRef surfaceOld, StringRef surfaceNew, StringRef traceStage,
      const RefoldModel::MacroInvocation *&directPasteChild,
      std::optional<DerivedConstraintList> &derivedConstraints) const {
    for (const auto *cand : children) {
      if (!cand || cand->pasteSpans.empty())
        continue;
      auto derived = deriveDirectPasteConstraints_(target, *cand, surfaceOld,
                                                   surfaceNew, traceStage);
      if (!derived)
        continue;
      if (directPasteChild) {
        if (directPasteChild != cand || !derivedConstraints ||
            !SameDerivedConstraints(*derivedConstraints, *derived))
          return false;
        continue;
      }
      directPasteChild = cand;
      derivedConstraints = std::move(derived);
    }
    return true;
  }

  const RefoldMacroDAGStructuredLifter &lifter_;
  const RefoldMacroDAGStructuredLifter::Dependencies &deps_;
  const RefoldMacroDAGLiftingContext &ctx_;
  DirectPasteDerivationFn deriveDirectPasteConstraints_;
};

} // namespace

RefoldMacroPasteArgumentBuilder
RefoldMacroDAGStructuredLifter::pasteArgumentBuilder() const {
  return RefoldMacroPasteArgumentBuilder(
      {&deps_.sourceMapper, deps_.aToks, deps_.bToks, &deps_.lexLang});
}

std::optional<DenseMap<uint32_t, FormalTextPair>>
RefoldMacroDAGStructuredLifter::TryLexicalChildBridge(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &parent,
    const RefoldModel::MacroInvocation &child,
    const std::string &rewrittenChildSyntax) const {
  DenseMap<uint32_t, FormalTextPair> out;

  // Empty child syntax cannot produce a meaningful parent-argument
  // rewrite.
  if (rewrittenChildSyntax.empty())
    return std::nullopt;

  std::optional<uint32_t> matchedFormal;
  std::optional<LexicalChildPlaceholder> matchedSlot;

  // Search every parent formal for a top-level lexical child placeholder
  // corresponding to the rewritten child invocation.
  for (uint32_t parentFormal = 0; parentFormal < parent.invArgRanges.size();
       ++parentFormal) {
    auto argInfo =
        deps_.textPrimitives.GetTrimmedInvocationArgInfo(parent, parentFormal);
    if (!argInfo)
      continue;

    auto slots = deps_.textPrimitives.GetTopLevelLexicalChildrenInArg(
        parent, parentFormal);
    for (const auto &slot : slots) {
      if (!slot.child || slot.child->id != child.id)
        continue;

      // The bridge must be unambiguous. If the same child can be
      // associated with multiple parent formals/slots, do not choose one
      // heuristically.
      if (matchedFormal)
        return std::nullopt;

      matchedFormal = parentFormal;
      matchedSlot = slot;
    }
  }

  if (!matchedFormal || !matchedSlot)
    return std::nullopt;

  auto argInfo =
      deps_.textPrimitives.GetTrimmedInvocationArgInfo(parent, *matchedFormal);
  if (!argInfo)
    return std::nullopt;

  // Revalidate the placeholder bounds against the trimmed parent argument
  // text before using them as replacement byte offsets.
  if (matchedSlot->relEnd < matchedSlot->relBegin ||
      matchedSlot->relEnd > argInfo->text.size())
    return std::nullopt;

  // Replace only the child placeholder inside the parent argument,
  // preserving the surrounding literal caller text.
  std::string rewrittenArg =
      stringutils::replaceRange(argInfo->text, matchedSlot->relBegin,
                                matchedSlot->relEnd, rewrittenChildSyntax);

  out[*matchedFormal] = FormalTextPair{StringRef(argInfo->text).trim().str(),
                                       StringRef(rewrittenArg).trim().str()};
  return out;
}

InvocationRewriteCertificate
RefoldMacroDAGStructuredLifter::BuildInvocationRewriteCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &inv,
    const DenseMap<uint32_t, FormalTextPair> &formals, StringRef traceStage,
    std::optional<StringRef> callsiteTextOverride,
    ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride,
    ArrayRef<uint32_t> deferOccurrenceArgIdxs) const {
  InvocationRewriteCertificate cert;
  cert.inv = &inv;

  // DenseMap iteration is intentionally unordered, so process formals in
  // sorted argument order for deterministic validation, diagnostics, and
  // replay state.
  SmallVector<uint32_t, 8> argOrder;
  argOrder.reserve(formals.size());
  for (const auto &kv : formals)
    argOrder.push_back(kv.first);
  llvm::sort(argOrder);

  for (uint32_t argIdx : argOrder) {
    auto it = formals.find(argIdx);
    if (it == formals.end())
      continue;

    StringRef oldText = StringRef(it->second.oldText).trim();
    StringRef newText = StringRef(it->second.newText).trim();

    // Some semantic paths already discharge occurrence consistency for a
    // formal through a stronger structural proof. Those arguments still
    // get arity and surface validation here, but the raw all-occurrences
    // check is deferred.
    const bool deferOccurrenceConsistency =
        llvm::is_contained(deferOccurrenceArgIdxs, argIdx);
    auto validation =
        deps_.invertibilitySolver.BuildRawFormalValidationCertificate(
            inv, argIdx, oldText, newText, traceStage, ctx.tokenHunksAR,
            deferOccurrenceConsistency);

    cert.formalValidations.push_back(validation);
    if (!validation.valid) {
      switch (validation.failure) {
      case RawFormalValidationFailure::ArityChange:
        cert.failure = InvocationRewriteFailure::ArityChange;
        break;
      case RawFormalValidationFailure::OccurrenceMismatch:
        cert.failure = InvocationRewriteFailure::OccurrenceMismatch;
        break;
      case RawFormalValidationFailure::None:
        cert.failure = InvocationRewriteFailure::None;
        break;
      }
      cert.detail = validation.detail;
      return cert;
    }

    // Carry every validated formal spelling into the replacement map,
    // including no-change formals. Paste replay may need unchanged paste
    // participants in order to reconstruct all pasted tokens.
    cert.replacementByArgIdx[argIdx] = newText.str();
    if (!cert.touchesPaste)
      cert.touchesPaste = invocationArgTouchesPaste(inv, argIdx);

    if (oldText == newText)
      continue;

    cert.rewrites.push_back(
        CertifiedFormalRewrite{argIdx, oldText.str(), newText.str()});
  }

  auto logInvocationSupportLedger = [&](StringRef ledgerState) {
    if (!inTraceMode())
      return;
    // Build the support ledger only when trace logging will consume it:
    // provided formals, actually changed formals, carried formals, and
    // paste-required formals that still lack replacement support.
    auto providedArgIdxs = argOrder;
    auto carriedArgIdxs = collectSortedUInt32Keys(cert.replacementByArgIdx);

    SmallVector<uint32_t, 8> changedArgIdxs;
    changedArgIdxs.reserve(cert.rewrites.size());
    for (const auto &rewrite : cert.rewrites)
      changedArgIdxs.push_back(rewrite.argIdx);
    llvm::sort(changedArgIdxs);

    auto requiredPasteArgIdxs = collectSortedUniquePasteArgIdxs(inv.pasteSpans);
    auto missingSupportArgIdxs =
        computeSortedMissingUInt32s(requiredPasteArgIdxs, carriedArgIdxs);

    trace("macro/proof",
          "{0}: invocation support ledger {1} inv id={2} name={3} "
          "provided={4} changed={5} carried={6} requiredPaste={7} "
          "missingSupport={8} deferredArgs={9} pasteRequired={10} "
          "pasteValid={11} pasteDeferred={12}",
          traceStage, ledgerState, inv.id, inv.name,
          formatUInt32List(providedArgIdxs), formatUInt32List(changedArgIdxs),
          formatUInt32List(carriedArgIdxs),
          formatUInt32List(requiredPasteArgIdxs),
          formatUInt32List(missingSupportArgIdxs),
          formatUInt32List(deferOccurrenceArgIdxs),
          cert.pasteValidation.required ? 1 : 0,
          cert.pasteValidation.valid ? 1 : 0,
          cert.pasteValidation.deferred ? 1 : 0);
  };

  logInvocationSupportLedger("enter");

  // If every validated formal collapsed to its original text, the
  // invocation has no rewrite to materialize. Still return a certificate
  // so callers can report the no-change proof path deterministically.
  if (cert.rewrites.empty()) {
    cert.kind = InvocationRewriteCertificateKind::NoChange;
    logInvocationSupportLedger("no-change");
    return cert;
  }

  // Paste validation must run after all formal replacements are known,
  // because a pasted token may depend on several formals and some of them
  // may be unchanged but still required for replay.
  cert.pasteValidation =
      deps_.invertibilitySolver.BuildPasteRewriteValidationCertificate(
          inv, cert.replacementByArgIdx, traceStage, callsiteTextOverride,
          callsiteArgRangesOverride);
  cert.touchesPaste = cert.pasteValidation.required;

  logInvocationSupportLedger("replay");

  if (!cert.pasteValidation.valid) {
    switch (cert.pasteValidation.failure) {
    case PasteRewriteValidationFailure::MissingInvocationText:
      cert.failure = InvocationRewriteFailure::MissingInvocationText;
      break;
    case PasteRewriteValidationFailure::MissingArgumentRanges:
      cert.failure = InvocationRewriteFailure::MissingArgumentRanges;
      break;
    case PasteRewriteValidationFailure::PasteMismatch:
      cert.failure = InvocationRewriteFailure::PasteMismatch;
      break;
    case PasteRewriteValidationFailure::None:
      cert.failure = InvocationRewriteFailure::None;
      break;
    }
    cert.detail = cert.pasteValidation.detail;
    return cert;
  }

  cert.kind = InvocationRewriteCertificateKind::Unique;
  return cert;
}

InvocationRewriteCertificate
RefoldMacroDAGStructuredLifter::BuildWrapperPlaceholderHopInvocationCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &inv,
    const DenseMap<uint32_t, FormalTextPair> &formals, StringRef traceStage,
    std::optional<StringRef> callsiteTextOverride,
    ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride,
    ArrayRef<uint32_t> deferOccurrenceArgIdxs) const {
  auto cert = this->BuildInvocationRewriteCertificate(
      ctx, inv, formals, traceStage, callsiteTextOverride,
      callsiteArgRangesOverride, deferOccurrenceArgIdxs);

  DenseMap<uint32_t, std::string> replByFormal;
  for (const auto &kv : formals) {
    StringRef oldText = StringRef(kv.second.oldText).trim();
    StringRef newText = StringRef(kv.second.newText).trim();
    if (oldText == newText)
      continue;
    replByFormal[kv.first] = newText.str();
  }

  // Materialize the rewritten invocation spelling from only the changed
  // formals. Wrapper-chain reconstruction needs this concrete call-site
  // syntax even when the normal proof certificate later needs paste
  // replay deferral.
  if (!replByFormal.empty()) {
    if (auto rewritten = deps_.textPrimitives.BuildRewrittenInvocationSyntax(
            inv, replByFormal))
      cert.rewrittenInvocationSyntax = std::move(*rewritten);
  }

  // In the ordinary case, return the normal certificate unchanged. The
  // special deferral below applies only to paste mismatch failures where
  // we successfully produced rewritten invocation syntax for this
  // placeholder-hop.
  if (cert.kind != InvocationRewriteCertificateKind::Invalid ||
      cert.failure != InvocationRewriteFailure::PasteMismatch ||
      cert.rewrittenInvocationSyntax.empty())
    return cert;

  // Placeholder-hop rewriting can temporarily break local paste replay
  // because the decisive validation happens after the parent wrapper
  // incorporates the rewritten child syntax. Mark that paste check as
  // deferred rather than rejected, while keeping the deferral explicit in
  // the certificate detail.
  cert.kind = InvocationRewriteCertificateKind::Unique;
  cert.failure = InvocationRewriteFailure::None;
  cert.pasteValidation.valid = true;
  cert.pasteValidation.deferred = true;
  cert.pasteValidation.failure = PasteRewriteValidationFailure::None;
  cert.detail = formatv("{0}: wrapper placeholder-hop paste validation "
                        "deferred: inv id={1} name={2} touchedArgs={3}",
                        traceStage, inv.id, inv.name, cert.rewrites.size())
                    .str();
  cert.pasteValidation.detail = cert.detail;
  return cert;
}

std::optional<DenseMap<uint32_t, FormalTextPair>>
RefoldMacroDAGStructuredLifter::
    BuildRootFormalRewriteMapFromCallsiteReplacement(
        const RefoldMacroDAGLiftingContext &ctx, StringRef baseText,
        StringRef newText) const {
  auto newRangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(ctx.m, newText);
  if (!newRangesOpt || newRangesOpt->size() != ctx.invArgRanges.size())
    return std::nullopt;

  DenseMap<uint32_t, FormalTextPair> formals;
  for (uint32_t argIdx = 0; argIdx < ctx.invArgRanges.size(); ++argIdx) {
    const auto &oldR = ctx.invArgRanges[argIdx];
    const auto &newR = (*newRangesOpt)[argIdx];

    // Both old and newly parsed ranges must be valid slices of their
    // respective invocation spellings before they can be compared as
    // formal arguments.
    if (oldR.first > oldR.second || oldR.second > baseText.size() ||
        newR.first > newR.second || newR.second > newText.size())
      return std::nullopt;

    StringRef oldArg =
        baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
    StringRef newArg =
        newText.slice((size_t)newR.first, (size_t)newR.second).trim();

    if (oldArg == newArg)
      continue;

    formals[argIdx] = FormalTextPair{oldArg.str(), newArg.str()};
  }

  return formals;
}

std::optional<SmallVector<std::pair<uint64_t, uint64_t>, 4>>
RefoldMacroDAGStructuredLifter::TryRebasePasteGroupToObservedSurface(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation *surfaceOwner,
    ArrayRef<const RefoldModel::PPArgSpan *> group, StringRef observedSurface,
    StringRef traceStage) const {
  SmallVector<std::pair<uint64_t, uint64_t>, 4> rebased;
  rebased.reserve(group.size());
  const uint64_t observedLen = observedSurface.size();

  // Fast path:
  // If every span already has a valid byte range fully inside the
  // current observed surface, then the group is already expressed in the
  // local coordinate space and does not need rebasing.
  bool fitsObservedSurface = true;
  for (const auto *sp : group) {
    if (!sp->byteBegin || !sp->byteEnd || *sp->byteBegin > *sp->byteEnd ||
        *sp->byteEnd > observedLen) {
      fitsObservedSurface = false;
      break;
    }
  }
  if (fitsObservedSurface) {
    for (const auto *sp : group)
      rebased.push_back({*sp->byteBegin, *sp->byteEnd});
    return rebased;
  }

  // If the spans do not already fit the observed surface, we can only
  // recover them if we know which enclosing invocation owns the larger
  // pasted token that these spans were originally measured against.
  if (!surfaceOwner)
    return std::nullopt;

  std::optional<uint64_t> base;
  std::optional<uint64_t> limit;
  const uint64_t tokBegin = group.front()->begin;
  const uint64_t tokEnd = group.front()->end;

  // Find the enclosing paste envelope on the surface owner for the same
  // emitted token `[tokBegin, tokEnd)`.
  //
  // Multiple owner spans may contribute to that token, so we compute the
  // minimal base and maximal limit across all matching owner paste spans.
  // The resulting [base, limit) interval is the full owner-local byte
  // range for the observed pasted surface.
  for (const auto &ownerSp : surfaceOwner->pasteSpans) {
    if (!ownerSp.byteBegin || !ownerSp.byteEnd)
      continue;
    if (ownerSp.begin != tokBegin || ownerSp.end != tokEnd)
      continue;
    base = base ? std::min<uint64_t>(*base, *ownerSp.byteBegin)
                : *ownerSp.byteBegin;
    limit =
        limit ? std::max<uint64_t>(*limit, *ownerSp.byteEnd) : *ownerSp.byteEnd;
  }

  // The enclosing owner envelope must:
  //   - exist
  //   - be well-formed
  //   - have width exactly equal to the current observed surface
  //
  // If not, we cannot safely interpret the child spans relative to the
  // local replay surface.
  if (!base || !limit || *limit < *base || (*limit - *base) != observedLen)
    return std::nullopt;

  // Rebase each child span from owner-local/full-token coordinates into
  // observed-surface-local coordinates by subtracting the enclosing base.
  //
  // Each span must lie fully inside the enclosing owner envelope;
  // otherwise the replay would be inconsistent and must be rejected.
  for (const auto *sp : group) {
    if (!sp->byteBegin || !sp->byteEnd)
      return std::nullopt;
    if (*sp->byteBegin < *base || *sp->byteEnd < *sp->byteBegin ||
        *sp->byteEnd > *limit)
      return std::nullopt;
    rebased.push_back({*sp->byteBegin - *base, *sp->byteEnd - *base});
  }

  return rebased;
}

std::optional<ParentConstraintDerivationCertificate>
RefoldMacroDAGStructuredLifter::TryBuildNestedPasteChainDerivation(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
    StringRef curOld, StringRef curNew, StringRef traceStage) const {
  auto argText = deps_.textPrimitives.GetInvocationArgText(cur, curFormal);
  if (!argText || argText->empty())
    return std::nullopt;

  ArrayRef<const RefoldModel::MacroInvocation *> children =
      deps_.macroTopology.MacroChildrenOf(cur.id);
  if (children.empty())
    return std::nullopt;

  // The current formal must name exactly one nested child invocation by
  // raw invocation spelling. Multiple exact matches would make the
  // paste-chain owner ambiguous.
  const RefoldModel::MacroInvocation *nested = nullptr;
  SmallVector<uint32_t, 4> nestedMatches;
  for (const auto *cand : children) {
    if (!cand || !cand->invText)
      continue;
    if (StringRef(*cand->invText).trim() != argText->trim())
      continue;
    nestedMatches.push_back(cand->id);
    if (nested) {
      REFOLD_LOG_TRACE("macro/dag",
                       "{0}: nested pasted-chain derivation ambiguous "
                       "nested child matches parent child id={1} name={2} "
                       "argIdx={3} matches={4}",
                       traceStage, cur.id, cur.name, curFormal,
                       formatUInt32List(nestedMatches));
      return std::nullopt;
    }
    nested = cand;
  }
  if (!nested) {
    return std::nullopt;
  }

  // The observed old token must correspond to exactly one old expansion
  // surface of the nested invocation. Otherwise the old side does not
  // identify a unique nested paste-token witness.
  SmallVector<std::string, 4> oldExpansionCandidates =
      deps_.textPrimitives.GetExpansionTextCandidates(*nested, /*fromB=*/false);
  SmallVector<std::string, 4> matchingOldCandidates;
  for (const auto &cand : oldExpansionCandidates) {
    if (StringRef(cand).trim() == curOld.trim())
      matchingOldCandidates.push_back(cand);
  }
  if (matchingOldCandidates.size() != 1)
    return std::nullopt;

  // Group paste spans by the PP token they produced. This derivation is
  // for one pasted token assembled from multiple formal contributions, so
  // require exactly one group below.
  DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
      pasteGroups;
  for (const auto &sp : nested->pasteSpans) {
    if (!sp.byteBegin || !sp.byteEnd)
      return std::nullopt;
    const uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
    pasteGroups[key].push_back(&sp);
  }
  if (pasteGroups.size() != 1)
    return std::nullopt;

  auto &group = pasteGroups.begin()->second;
  if (group.size() < 2)
    return std::nullopt;

  llvm::sort(group, pasteSpanPtrLessByByteRange);

  StringRef oldTok = curOld.trim();
  StringRef newTok = curNew.trim();
  if (oldTok.empty() || newTok.empty())
    return std::nullopt;

  // Rebase producer paste byte ranges onto the observed old token
  // surface. The nested child may have been observed through a wrapper,
  // so the producer-local byte windows must be aligned to `oldTok` before
  // using them to split `newTok`.
  auto rebasedGroup = this->TryRebasePasteGroupToObservedSurface(
      ctx, &cur, ArrayRef<const RefoldModel::PPArgSpan *>(group), oldTok,
      traceStage);
  if (!rebasedGroup)
    return std::nullopt;
  for (size_t i = 1; i < rebasedGroup->size(); ++i) {
    if ((*rebasedGroup)[i - 1].second > (*rebasedGroup)[i].first)
      return std::nullopt;
  }

  // Text outside the first/last paste segments is stable boundary text.
  // The new pasted token must preserve it before we try to split the
  // changed core.
  StringRef leading = oldTok.take_front((*rebasedGroup).front().first);
  StringRef trailing = oldTok.drop_front((*rebasedGroup).back().second);
  if (!newTok.starts_with(leading) || !newTok.ends_with(trailing))
    return std::nullopt;

  SmallVector<StringRef, 4> oldSegs;
  SmallVector<StringRef, 4> midBodies;
  oldSegs.reserve(group.size());
  midBodies.reserve(group.size() - 1);
  for (size_t i = 0; i < group.size(); ++i) {
    const auto [segBegin, segEnd] = (*rebasedGroup)[i];
    oldSegs.push_back(oldTok.slice(segBegin, segEnd));
    if (i + 1 < group.size()) {
      StringRef mid = oldTok.slice(segEnd, (*rebasedGroup)[i + 1].first);
      if (mid.empty())
        return std::nullopt;
      midBodies.push_back(mid);
    }
  }

  StringRef core =
      newTok.slice(leading.size(), newTok.size() - trailing.size());

  // Accept only a unique split with one new segment for each old paste
  // contribution. Anything else is ambiguous or structurally incomplete.
  auto splitSolution = splitPastedCoreByDelimiters(core, oldSegs, midBodies);
  if (!splitSolution || splitSolution->size() != group.size())
    return std::nullopt;

  ParentConstraintDerivationCertificate cert;
  cert.childFormal = curFormal;
  DenseMap<uint32_t, ObservedFormalConstraint> mergedByParentFormal;

  // Recursively lift each nested paste operand rewrite into constraints
  // on the parent formal(s). If two nested operands derive different
  // constraints for the same parent formal, fail closed.
  for (size_t i = 0; i < group.size(); ++i) {
    const uint32_t nestedFormal = group[i]->argIdx;
    auto nestedCert = this->BuildParentConstraintDerivationCertificate(
        ctx, *nested, nestedFormal, oldSegs[i], (*splitSolution)[i],
        traceStage);
    if (!nestedCert.valid)
      return std::nullopt;
    for (const auto &derived : nestedCert.derivedConstraints) {
      auto itExisting = mergedByParentFormal.find(derived.first);
      if (itExisting == mergedByParentFormal.end()) {
        mergedByParentFormal.insert({derived.first, derived.second});
        continue;
      }
      if (itExisting->second.oldText != derived.second.oldText ||
          itExisting->second.newText != derived.second.newText)
        return std::nullopt;
    }
  }

  for (const auto &kv : mergedByParentFormal)
    cert.derivedConstraints.push_back({kv.first, kv.second});
  llvm::sort(cert.derivedConstraints,
             [](const auto &a, const auto &b) { return a.first < b.first; });
  if (cert.derivedConstraints.empty())
    return std::nullopt;
  cert.valid = true;
  return cert;
}

std::optional<ParentConstraintDerivationCertificate>
RefoldMacroDAGStructuredLifter::TryBuildTwoParentDelimitedDerivation(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
    StringRef curOld, StringRef curNew, StringRef traceStage) const {
  if (!cur.callerMacroId || curFormal >= cur.argDeps.size())
    return std::nullopt;

  auto parentIt =
      ctx.subtreeValidationCtx.invocationById.find(*cur.callerMacroId);
  if (parentIt == ctx.subtreeValidationCtx.invocationById.end())
    return std::nullopt;
  const RefoldModel::MacroInvocation *parent = parentIt->second;
  ArrayRef<uint32_t> deps = cur.argDeps[curFormal];

  // This derivation is intentionally limited to a binary dependency:
  // one child formal assembled from exactly two parent formals.
  if (deps.size() != 2)
    return std::nullopt;

  auto oldAOpt = deps_.textPrimitives.GetInvocationArgText(*parent, deps[0]);
  auto oldBOpt = deps_.textPrimitives.GetInvocationArgText(*parent, deps[1]);
  if (!oldAOpt || !oldBOpt)
    return std::nullopt;

  const StringRef oldTok = curOld.trim();
  const StringRef newTok = curNew.trim();
  const StringRef oldA = oldAOpt->trim();
  const StringRef oldB = oldBOpt->trim();
  if (oldTok.empty() || newTok.empty() || oldA.empty() || oldB.empty())
    return std::nullopt;

  // Prove that the old child surface is exactly oldA + mid + oldB. If
  // the two parent texts are not anchored at the edges, there is no stable
  // middle delimiter to reuse when splitting the new child surface.
  if (!oldTok.starts_with(oldA) || !oldTok.ends_with(oldB) ||
      oldTok.size() < oldA.size() + oldB.size()) {
    return std::nullopt;
  }

  StringRef mid = oldTok.slice(oldA.size(), oldTok.size() - oldB.size());
  if (mid.empty())
    return std::nullopt;

  // If oldA or oldB themselves contain the delimiter, a valid split of the
  // new text must leave those delimiter occurrences on the corresponding
  // side. Otherwise the split would steal text that belongs inside a
  // parent argument.
  const uint64_t needA = countSubstringOccurrences(oldA, mid);
  const uint64_t needB = countSubstringOccurrences(oldB, mid);

  SmallVector<std::pair<StringRef, StringRef>, 4> splits;
  for (size_t pos = 0; (pos = newTok.find(mid, pos)) != StringRef::npos;
       ++pos) {
    StringRef newA = newTok.slice(0, pos);
    StringRef newB = newTok.drop_front(pos + mid.size());
    if (countSubstringOccurrences(newA, mid) < needA ||
        countSubstringOccurrences(newB, mid) < needB)
      continue;
    splits.push_back({newA, newB});
  }

  // The delimiter must determine exactly one newA/newB split. No split
  // means the shape was not preserved; multiple splits are ambiguous.
  if (splits.size() != 1)
    return std::nullopt;

  ParentConstraintDerivationCertificate cert;
  cert.childFormal = curFormal;
  cert.valid = true;

  // Lift the unique child split into one observed rewrite constraint for
  // each parent formal.
  cert.derivedConstraints.push_back(
      {deps[0],
       ObservedFormalConstraint{oldA.str(), splits[0].first.trim().str()}});
  cert.derivedConstraints.push_back(
      {deps[1],
       ObservedFormalConstraint{oldB.str(), splits[0].second.trim().str()}});
  cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} accepted "
                        "via two-parent delimited derivation",
                        traceStage, cur.id, cur.name, curFormal)
                    .str();
  return cert;
}

ParentConstraintDerivationCertificate
RefoldMacroDAGStructuredLifter::BuildParentConstraintDerivationCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
    StringRef curOld, StringRef curNew, StringRef traceStage) const {
  ParentConstraintDerivationCertificate cert;
  cert.childFormal = curFormal;

  // `argDeps` says which parent formals flow into this child formal. If
  // the dependency metadata is missing or empty, arg-ref inversion cannot
  // prove the parent constraints.
  if (curFormal >= cur.argDeps.size()) {
    cert.failure = ParentConstraintDerivationFailure::MissingArgDeps;
    cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} missing "
                          "argDeps entry; lexical bridge required",
                          traceStage, cur.id, cur.name, curFormal)
                      .str();
    return cert;
  }
  ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
  if (deps.empty()) {
    cert.failure = ParentConstraintDerivationFailure::EmptyArgDeps;
    cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} has "
                          "empty argDeps; lexical bridge required",
                          traceStage, cur.id, cur.name, curFormal)
                      .str();
    return cert;
  }

  // `argRefs` gives byte-level placeholders inside the child argument.
  // Without it, we know a dependency exists but cannot split the observed
  // child old/new text back into parent-formal slices.
  if (curFormal >= cur.argRefs.size()) {
    cert.failure = ParentConstraintDerivationFailure::MissingArgRefs;
    cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} missing "
                          "argRefs entry; lexical bridge required",
                          traceStage, cur.id, cur.name, curFormal)
                      .str();
    return cert;
  }

  auto tpl = deps_.textPrimitives.BuildArgRefTemplate(cur, curFormal);
  if (!tpl || tpl->refs.empty() ||
      !deps_.invertibilitySolver.SameIndexSet(deps,
                                              tpl->distinctCallerParams)) {
    // If the generic arg-ref template is unavailable, try the two narrow
    // structural derivations that can still prove parent constraints
    // without a normal placeholder template.
    if (auto twoParentCert = this->TryBuildTwoParentDelimitedDerivation(
            ctx, cur, curFormal, curOld, curNew, traceStage)) {
      return *twoParentCert;
    }
    if (auto nestedCert = this->TryBuildNestedPasteChainDerivation(
            ctx, cur, curFormal, curOld, curNew, traceStage)) {
      return *nestedCert;
    }
    cert.failure = ParentConstraintDerivationFailure::TemplateNotCertifiable;
    cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} arg-ref "
                          "template not certifiable; lexical bridge "
                          "required",
                          traceStage, cur.id, cur.name, curFormal)
                      .str();
    return cert;
  }

  // Invert both the old and new observed child text through the same
  // template. Both sides must produce a unique assignment from parent
  // formal -> observed slice, or the lifted parent rewrite is ambiguous.
  auto oldCert = deps_.invertibilitySolver.BuildArgRefInvertibilityCertificate(
      *tpl, curOld);
  auto newCert = deps_.invertibilitySolver.BuildArgRefInvertibilityCertificate(
      *tpl, curNew);
  if (oldCert.kind != ArgRefInvertibilityKind::Unique ||
      newCert.kind != ArgRefInvertibilityKind::Unique) {
    if (auto twoParentCert = this->TryBuildTwoParentDelimitedDerivation(
            ctx, cur, curFormal, curOld, curNew, traceStage)) {
      return *twoParentCert;
    }
    if (auto nestedCert = this->TryBuildNestedPasteChainDerivation(
            ctx, cur, curFormal, curOld, curNew, traceStage)) {
      return *nestedCert;
    }
    cert.failure = ParentConstraintDerivationFailure::InversionNotUnique;
    cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} arg-ref "
                          "inversion not unique; lexical bridge required",
                          traceStage, cur.id, cur.name, curFormal)
                      .str();
    return cert;
  }

  // Pair the old and new assignments for every parent formal referenced
  // by the template. Missing either side means the child observation did
  // not derive a complete parent-level constraint.
  for (uint32_t parentFormal : tpl->distinctCallerParams) {
    auto oldIt = oldCert.derivedTextByCallerParam.find(parentFormal);
    auto newIt = newCert.derivedTextByCallerParam.find(parentFormal);
    if (oldIt == oldCert.derivedTextByCallerParam.end() ||
        newIt == newCert.derivedTextByCallerParam.end()) {
      cert.failure = ParentConstraintDerivationFailure::IncompleteDerivation;
      cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} "
                            "parent formal derivation incomplete; lexical "
                            "bridge required",
                            traceStage, cur.id, cur.name, curFormal)
                        .str();
      cert.derivedConstraints.clear();
      return cert;
    }
    cert.derivedConstraints.push_back(
        {parentFormal, ObservedFormalConstraint{oldIt->second, newIt->second}});
  }

  cert.valid = true;
  return cert;
}

std::optional<StructuredLiftCertificate>
RefoldMacroDAGStructuredLifter::TryBuildExactSiblingRerootLift(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &parent,
    const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
    StringRef curOld, StringRef curNew) const {
  const StringRef oldTrim = curOld.trim();
  const StringRef newTrim = curNew.trim();
  if (oldTrim.empty() || newTrim.empty())
    return std::nullopt;

  const RefoldModel::MacroInvocation *matchedSibling = nullptr;

  // Find the unique sibling invocation under the same parent whose raw
  // invocation spelling exactly matches the old observed child surface.
  // Multiple siblings with the same spelling would make the reroot target
  // ambiguous.
  for (const auto &cand : deps_.model.GetMacroInvocations()) {
    if (cand.id == cur.id || !cand.callerMacroId ||
        *cand.callerMacroId != parent.id || !cand.invText)
      continue;
    if (StringRef(*cand.invText).trim() != oldTrim)
      continue;
    if (matchedSibling) {
      return std::nullopt;
    }
    matchedSibling = &cand;
  }

  if (!matchedSibling || matchedSibling->invArgRanges.empty()) {
    return std::nullopt;
  }

  std::optional<StructuredLiftCertificate> uniqueLift;
  std::optional<uint32_t> uniqueSiblingFormal;

  auto sameNextFormals = [&](const DenseMap<uint32_t, FormalTextPair> &lhs,
                             const DenseMap<uint32_t, FormalTextPair> &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &kvLocal : lhs) {
      auto it = rhs.find(kvLocal.first);
      if (it == rhs.end())
        return false;
      if (it->second.oldText != kvLocal.second.oldText ||
          it->second.newText != kvLocal.second.newText)
        return false;
    }
    return true;
  };

  struct ConcreteExemplarReplayLiftResult {
    enum class State {
      None,
      Unique,
      Ambiguous,
    };

    State state = State::None;
    std::optional<StructuredLiftCertificate> lift;
  };

  auto tryBuildConcreteExemplarReplayLift =
      [&](uint32_t siblingFormal,
          StringRef siblingOldTrim) -> ConcreteExemplarReplayLiftResult {
    SmallVector<std::string, 4> parentActuals;
    for (uint32_t parentFormal = 0; parentFormal < parent.invArgRanges.size();
         ++parentFormal) {
      if (auto parentArg =
              deps_.textPrimitives.GetInvocationArgText(parent, parentFormal)) {
        StringRef parentArgTrim = parentArg->trim();
        if (!parentArgTrim.empty() &&
            !containsSpelling(parentActuals, parentArgTrim))
          parentActuals.push_back(parentArgTrim.str());
      }
    }

    ConcreteExemplarReplayLiftResult result;
    for (const auto &exemplar : deps_.model.GetMacroInvocations()) {
      if (exemplar.id == matchedSibling->id ||
          exemplar.name != matchedSibling->name ||
          siblingFormal >= exemplar.invArgRanges.size())
        continue;

      auto exemplarOldArg =
          deps_.textPrimitives.GetInvocationArgText(exemplar, siblingFormal);
      if (!exemplarOldArg)
        continue;
      StringRef exemplarOldTrim = exemplarOldArg->trim();
      if (exemplarOldTrim.empty() ||
          !containsSpelling(parentActuals, exemplarOldTrim))
        continue;

      // Use another invocation of the same sibling macro as a concrete
      // exemplar. If replacing the exemplar's old formal with the new
      // observed surface derives exactly one concrete new formal value,
      // replay that value through the matched sibling.
      SmallVector<std::string, 4> projectedConcreteNews;
      for (const auto &oldExpStr :
           deps_.textPrimitives.GetExpansionTextCandidates(exemplar,
                                                           /*fromB=*/false)) {
        StringRef projected =
            pasteArgumentBuilder().DeriveNewPasteSegmentFromSpellingReplacement(
                StringRef(oldExpStr).trim(), newTrim, exemplarOldTrim);
        projected = projected.trim();
        if (projected.empty() || projected == exemplarOldTrim)
          continue;
        if (!containsSpelling(projectedConcreteNews, projected))
          projectedConcreteNews.push_back(projected.str());
      }
      if (projectedConcreteNews.size() != 1)
        continue;

      const std::string &projectedConcreteNew = projectedConcreteNews.front();

      DenseMap<uint32_t, FormalTextPair> exemplarFormals;
      exemplarFormals[siblingFormal] =
          FormalTextPair{exemplarOldTrim.str(), projectedConcreteNew};
      auto exemplarInvCert =
          this->BuildWrapperPlaceholderHopInvocationCertificate(
              ctx, exemplar, exemplarFormals,
              "DAG per-hop exact sibling reroot concrete exemplar");
      if (exemplarInvCert.kind == InvocationRewriteCertificateKind::Invalid)
        continue;

      DenseMap<uint32_t, FormalTextPair> replayFormals;
      replayFormals[siblingFormal] =
          FormalTextPair{siblingOldTrim.str(), projectedConcreteNew};
      auto replayInvCert =
          this->BuildWrapperPlaceholderHopInvocationCertificate(
              ctx, *matchedSibling, replayFormals,
              "DAG per-hop exact sibling reroot concrete replay");
      if (replayInvCert.kind == InvocationRewriteCertificateKind::Invalid)
        continue;

      auto projectedLift = this->BuildStructuredLiftCertificate(
          ctx, *matchedSibling, replayFormals);
      if (projectedLift.kind != StructuredLiftCertificateKind::Unique ||
          projectedLift.nextInv != &parent)
        continue;

      // Multiple exemplars are acceptable only if they produce the same
      // parent-formal rewrite. Divergent projections make the reroot
      // lift ambiguous.
      if (result.lift) {
        if (!sameNextFormals(result.lift->nextFormals,
                             projectedLift.nextFormals)) {
          result.state = ConcreteExemplarReplayLiftResult::State::Ambiguous;
          result.lift.reset();
          return result;
        }
        result.state = ConcreteExemplarReplayLiftResult::State::Unique;
        continue;
      }

      result.state = ConcreteExemplarReplayLiftResult::State::Unique;
      result.lift = std::move(projectedLift);
    }

    return result;
  };

  for (uint32_t siblingFormal = 0;
       siblingFormal < matchedSibling->invArgRanges.size(); ++siblingFormal) {
    auto siblingOldArg = deps_.textPrimitives.GetInvocationArgText(
        *matchedSibling, siblingFormal);
    if (!siblingOldArg) {
      continue;
    }

    const StringRef siblingOldTrim = siblingOldArg->trim();
    if (siblingOldTrim.empty() || siblingOldTrim == newTrim) {
      continue;
    }

    DenseMap<uint32_t, FormalTextPair> siblingFormals;
    siblingFormals[siblingFormal] =
        FormalTextPair{siblingOldTrim.str(), newTrim.str()};
    auto siblingLift = this->BuildStructuredLiftCertificate(
        ctx, *matchedSibling, siblingFormals);
    if (siblingLift.kind != StructuredLiftCertificateKind::Unique ||
        siblingLift.nextInv != &parent)
      continue;

    auto siblingLiftHasCertifiedParentFormalEvidence = [&]() {
      for (const auto &derived : siblingLift.nextFormals) {
        const uint32_t parentFormal = derived.first;
        bool certified = false;
        for (const auto &formalCert : siblingLift.parentFormalCertificates) {
          if (formalCert.argIdx != parentFormal)
            continue;
          if (formalCert.kind != FormalRewriteCertificateKind::Invalid) {
            certified = true;
            break;
          }
        }
        if (!certified)
          return false;
      }
      return true;
    };

    if (!siblingLiftHasCertifiedParentFormalEvidence()) {
      auto replayResult =
          tryBuildConcreteExemplarReplayLift(siblingFormal, siblingOldTrim);
      if (replayResult.state ==
              ConcreteExemplarReplayLiftResult::State::Unique &&
          replayResult.lift) {
        siblingLift = std::move(*replayResult.lift);
      } else if (replayResult.state ==
                     ConcreteExemplarReplayLiftResult::State::None &&
                 siblingLift.parentCert.kind !=
                     InvocationRewriteCertificateKind::Invalid &&
                 siblingLift.parentInvocationFailure ==
                     InvocationRewriteFailure::None) {
        // The direct sibling lift already has a valid parent invocation
        // certificate, and no concrete exemplar contradicted it. Keep
        // the direct lift even though it lacks per-formal evidence.
      } else {
        continue;
      }
    }

    // Accept exactly one sibling formal as the reroot seed. If two
    // sibling formals both lift to the parent, the old->new relationship
    // is ambiguous.
    if (uniqueLift) {
      return std::nullopt;
    }

    uniqueSiblingFormal = siblingFormal;
    uniqueLift = std::move(siblingLift);
  }

  if (!uniqueLift || !uniqueSiblingFormal)
    return std::nullopt;

  return std::move(*uniqueLift);
}

StructuredLiftCertificate
RefoldMacroDAGStructuredLifter::BuildStructuredLiftCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &cur,
    const DenseMap<uint32_t, FormalTextPair> &curFormals) const {
  StructuredLiftCertificate cert;

  // Parent-formal observations are formatted deterministically for trace
  // output because DenseMap iteration order is unstable. A parent formal
  // may accumulate constraints from multiple child formals before the
  // certificate checks whether those observations are mutually consistent.
  //
  // First prove that the current invocation itself can be reconstructed
  // from the already-derived formal rewrites. If this fails, there is no
  // structured child syntax to lift through the parent.
  auto curCert = this->BuildWrapperPlaceholderHopInvocationCertificate(
      ctx, cur, curFormals, "DAG per-hop");
  cert.currentCert = curCert;
  if (curCert.kind == InvocationRewriteCertificateKind::Invalid) {
    cert.failureReason = StructuredLiftFailureReason::CurrentInvocationInvalid;
    cert.currentInvocationFailure = curCert.failure;
    cert.detail = curCert.detail;
    return cert;
  }

  // Materialize the rewritten child invocation syntax for two purposes:
  // logging and, when needed, as preferred syntax for a parent formal
  // that contains this child invocation as a lexical placeholder.
  DenseMap<uint32_t, std::string> curFormalSyntax;
  for (const auto &kvLocal : curFormals)
    curFormalSyntax[kvLocal.first] = kvLocal.second.newText;
  if (!curCert.rewrittenInvocationSyntax.empty()) {
    cert.rewrittenChildSyntax = curCert.rewrittenInvocationSyntax;
  } else if (auto curSyntax =
                 deps_.textPrimitives.BuildRewrittenInvocationSyntax(
                     cur, curFormalSyntax)) {
    cert.rewrittenChildSyntax = std::move(*curSyntax);
  }

  // A structured lift step normally moves from a child invocation to its
  // caller. If the child has no caller metadata, the only sound next step
  // is to ask the outer algorithm to bridge lexically back to the root.
  const RefoldModel::MacroInvocation *parent = nullptr;
  if (cur.callerMacroId) {
    auto parentIt =
        ctx.subtreeValidationCtx.invocationById.find(*cur.callerMacroId);
    if (parentIt == ctx.subtreeValidationCtx.invocationById.end()) {
      cert.failureReason = StructuredLiftFailureReason::MissingCallerInvocation;
      cert.detail =
          formatv("DAG per-hop: missing caller invocation: child id={0} "
                  "name={1} callerId={2}",
                  cur.id, cur.name, *cur.callerMacroId)
              .str();
      return cert;
    }
    parent = parentIt->second;
  } else {
    cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
    cert.failureReason = StructuredLiftFailureReason::RootLexicalBridgeRequired;
    cert.nextInv = &ctx.m;
    cert.detail = formatv("DAG per-hop: child id={0} name={1} requires "
                          "lexical bridge to root",
                          cur.id, cur.name)
                      .str();
    return cert;
  }

  /// Try to explain an observed rewrite of a pasted surface by replaying
  /// exactly one direct paste-producing child invocation.
  ///
  /// This helper is intentionally narrow. It only succeeds when:
  ///
  /// * the child has exactly one paste product at this hop,
  /// * the pasted operands can be rebased onto the observed surface,
  /// * the rewritten surface has a unique split around the original
  ///   inter-operand delimiters, and
  /// * every recovered operand rewrite can be lifted through the child's
  ///   normal formal-derivation certificate.
  ///
  /// Any ambiguity is rejected because it would amount to inventing an
  /// inverse paste decomposition rather than proving one.
  auto tryDeriveObservedConstraintsFromDirectPasteChild =
      [&](const RefoldModel::MacroInvocation &surfaceOwner,
          const RefoldModel::MacroInvocation &directChild,
          StringRef observedOld0, StringRef observedNew0, StringRef traceStage)
      -> std::optional<
          SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>> {
    StringRef observedOld = observedOld0.trim();
    StringRef observedNew = observedNew0.trim();
    if (observedOld.empty() || observedNew.empty())
      return std::nullopt;

    // Group spans by their common pasted result. Exact-shape replay only
    // handles a single pasted product at this hop; multiple independent
    // paste groups would require choosing between distinct replay
    // regions.
    DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
        pasteGroups;
    for (const auto &sp : directChild.pasteSpans) {
      if (!sp.byteBegin || !sp.byteEnd)
        return std::nullopt;
      const uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
      pasteGroups[key].push_back(&sp);
    }
    if (pasteGroups.size() != 1)
      return std::nullopt;

    auto &group = pasteGroups.begin()->second;
    if (group.size() < 2)
      return std::nullopt;

    llvm::sort(group, pasteSpanPtrLessByByteRange);

    // Rebase the child's paste operand byte ranges onto the observed
    // surface. This proves that the pasted product being edited is the
    // same concrete surface produced by this direct child.
    auto rebasedGroup = this->TryRebasePasteGroupToObservedSurface(
        ctx, &surfaceOwner, ArrayRef<const RefoldModel::PPArgSpan *>(group),
        observedOld, traceStage);
    if (!rebasedGroup)
      return std::nullopt;
    for (size_t i = 1; i < rebasedGroup->size(); ++i) {
      if ((*rebasedGroup)[i - 1].second > (*rebasedGroup)[i].first)
        return std::nullopt;
    }

    // The edit must preserve the non-pasted prefix/suffix verbatim.
    // Otherwise we are no longer replaying the same direct pasted child.
    StringRef leading = observedOld.take_front((*rebasedGroup).front().first);
    StringRef trailing = observedOld.drop_front((*rebasedGroup).back().second);
    if (!observedNew.starts_with(leading) || !observedNew.ends_with(trailing))
      return std::nullopt;

    // Extract the original operand surfaces and the literal material that
    // appeared between adjacent operands. The inter-operand material is
    // used below as the only permitted delimiter for splitting the new
    // pasted core.
    SmallVector<StringRef, 4> oldSegs;
    SmallVector<StringRef, 4> midBodies;
    oldSegs.reserve(group.size());
    midBodies.reserve(group.size() - 1);
    for (size_t i = 0; i < group.size(); ++i) {
      const auto [segBegin, segEnd] = (*rebasedGroup)[i];
      oldSegs.push_back(observedOld.slice(segBegin, segEnd));
      if (i + 1 < group.size()) {
        StringRef mid = observedOld.slice(segEnd, (*rebasedGroup)[i + 1].first);
        if (mid.empty())
          return std::nullopt;
        midBodies.push_back(mid);
      }
    }

    StringRef core =
        observedNew.slice(leading.size(), observedNew.size() - trailing.size());

    // Split the rewritten pasted core around the original inter-operand
    // delimiters. We only accept a unique segmentation; if multiple splits
    // work, the inverse-paste explanation is ambiguous and therefore not a
    // valid replay certificate.
    auto splitSolution = splitPastedCoreByDelimiters(core, oldSegs, midBodies);
    if (!splitSolution || splitSolution->size() != group.size())
      return std::nullopt;

    // Lift each recovered child-operand rewrite through the child's
    // normal parent-constraint derivation, then merge the resulting
    // parent-formal constraints. Conflicting lifts mean the pasted
    // surface cannot be explained by one consistent replay of the
    // original child.
    DenseMap<uint32_t, ObservedFormalConstraint> mergedByFormal;
    for (size_t i = 0; i < group.size(); ++i) {
      const uint32_t childFormal = group[i]->argIdx;
      auto derived = this->BuildParentConstraintDerivationCertificate(
          ctx, directChild, childFormal, oldSegs[i], (*splitSolution)[i],
          traceStage);
      if (!derived.valid)
        return std::nullopt;
      for (const auto &kv : derived.derivedConstraints) {
        auto itExisting = mergedByFormal.find(kv.first);
        if (itExisting == mergedByFormal.end()) {
          mergedByFormal.insert({kv.first, kv.second});
          continue;
        }
        if (itExisting->second.oldText != kv.second.oldText ||
            itExisting->second.newText != kv.second.newText)
          return std::nullopt;
      }
    }

    // Return a stable, sorted set of derived parent-formal observations.
    SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4> out;
    for (const auto &kv : mergedByFormal)
      out.push_back({kv.first, kv.second});
    llvm::sort(out,
               [](const auto &a, const auto &b) { return a.first < b.first; });
    if (out.empty())
      return std::nullopt;
    return out;
  };

  // Rebuild exact original nested paste syntax through a named resolver so the
  // recursive proof search has explicit state and keeps the same child-order,
  // ambiguity, and wrapper-placeholder proof obligations as the previous local
  // lambda.
  ExactOriginalShapePasteReplayResolver exactOriginalShapePasteReplayResolver(
      *this, deps_, ctx, tryDeriveObservedConstraintsFromDirectPasteChild);

  /// Handle the common DAG hop where one child formal maps directly to
  /// one parent formal.
  ///
  /// The normal result is a passthrough rewrite of the parent formal. If
  /// the child observed a flattened surface but the parent logical
  /// argument still contains one nested lexical child, this probes the
  /// exact-shape replay path first so that a provable nested paste tree
  /// is preserved rather than replaced by its materialized token text.
  auto tryBuildDirectPassthroughParentFormalRewrite =
      [&](uint32_t curFormal, uint32_t parentFormal,
          StringRef curNewText) -> std::optional<FormalTextPair> {
    if (curFormal >= cur.argDeps.size())
      return std::nullopt;
    ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
    if (deps.size() != 1 || deps[0] != parentFormal)
      return std::nullopt;

    auto tpl = deps_.textPrimitives.BuildArgRefTemplate(cur, curFormal);
    if (!tpl || tpl->refs.size() != 1 || tpl->distinctCallerParams.size() != 1)
      return std::nullopt;

    const auto &ref = tpl->refs[0];
    if (ref.callerParamIndex != parentFormal || ref.begin != 0 ||
        ref.end != StringRef(tpl->argText).trim().size())
      return std::nullopt;

    auto parentArgText =
        deps_.textPrimitives.GetInvocationArgText(*parent, parentFormal);
    if (!parentArgText)
      return std::nullopt;

    StringRef oldTrim = parentArgText->trim();
    StringRef newTrim = curNewText.trim();
    if (newTrim.empty() || oldTrim == newTrim)
      return std::nullopt;

    // `curFormals` may not carry this formal when the child rewrite was
    // derived through a different certified path, so guard the lookup.
    const auto curFormalIt = curFormals.find(curFormal);
    if (curFormalIt == curFormals.end())
      return std::nullopt;

    const StringRef childObservedOld =
        StringRef(curFormalIt->second.oldText).trim();

    // A mismatch here means the child has already collapsed some nested
    // structure relative to the parent's logical argument. If the parent
    // argument is exactly one lexical child, try to replay that original
    // nested shape instead of committing to the flatter replacement text.
    if (childObservedOld != oldTrim) {
      auto placeholders = deps_.textPrimitives.GetTopLevelLexicalChildrenInArg(
          *parent, parentFormal);
      if (placeholders.size() == 1 && placeholders.front().child &&
          placeholders.front().relBegin == 0 &&
          placeholders.front().relEnd == oldTrim.size()) {
        const RefoldModel::MacroInvocation *nestedChild =
            placeholders.front().child;
        if (auto replaySyntax = exactOriginalShapePasteReplayResolver.Resolve(
                *nestedChild, childObservedOld, newTrim,
                "DAG per-hop exact original-shape replay")) {
          return FormalTextPair{oldTrim.str(), std::move(*replaySyntax)};
        }
      }
    }

    return FormalTextPair{oldTrim.str(), newTrim.str()};
  };

  // Parent observations are the inverted constraints this hop derives
  // from child-formal rewrites. `parentObservedSources` tracks which
  // child formals contributed each parent observation so that narrowly
  // scoped recovery paths can prove they are not merging unrelated input.
  DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>> parentObserved;
  DenseMap<uint32_t, SmallVector<uint32_t, 2>> parentObservedSources;
  SmallVector<uint32_t, 4> unresolvedChildFormals;
  SmallVector<std::string, 4> unresolvedDerivationDetails;

  auto recordObservedParentConstraint =
      [&](uint32_t parentFormal, const ObservedFormalConstraint &constraint,
          uint32_t sourceCurFormal) {
        auto &constraints = parentObserved[parentFormal];
        bool seen = false;
        for (const auto &existing : constraints) {
          if (existing.oldText == constraint.oldText &&
              existing.newText == constraint.newText) {
            seen = true;
            break;
          }
        }
        if (!seen)
          constraints.push_back(constraint);

        auto &sources = parentObservedSources[parentFormal];
        if (llvm::find(sources, sourceCurFormal) == sources.end())
          sources.push_back(sourceCurFormal);
      };

  // Invert each child-formal rewrite through the current invocation's
  // formal dependency certificate. Failed inversions are not immediately
  // fatal because a sibling reroot or parent body-space proof may still
  // discharge the same obligation without requiring a lexical bridge.
  for (const auto &kvLocal : curFormals) {
    const uint32_t curFormal = kvLocal.first;
    StringRef curOld = kvLocal.second.oldText;
    StringRef curNew = kvLocal.second.newText;

    auto derivationCert = this->BuildParentConstraintDerivationCertificate(
        ctx, cur, curFormal, curOld, curNew, "DAG per-hop");
    cert.derivations.push_back(derivationCert);
    if (!derivationCert.valid) {
      if (parent) {
        // Some failed direct inversions are still exactly explainable by
        // rerooting through a sibling child under the same parent. Accept
        // only if that reroot produces concrete parent-formal rewrites.
        auto siblingLift = this->TryBuildExactSiblingRerootLift(
            ctx, *parent, cur, curFormal, curOld, curNew);
        if (siblingLift) {
          for (const auto &derived : siblingLift->nextFormals) {
            const uint32_t parentFormal = derived.first;
            const ObservedFormalConstraint constraint{derived.second.oldText,
                                                      derived.second.newText};
            recordObservedParentConstraint(parentFormal, constraint, curFormal);
          }
          continue;
        }
      }

      unresolvedChildFormals.push_back(curFormal);
      unresolvedDerivationDetails.push_back(derivationCert.detail);
      continue;
    }

    for (const auto &derived : derivationCert.derivedConstraints) {
      const uint32_t parentFormal = derived.first;
      const ObservedFormalConstraint &constraint = derived.second;
      recordObservedParentConstraint(parentFormal, constraint, curFormal);
    }
  }

  // Prefer the already-certified child invocation syntax when proving a
  // parent formal that contains this child as a placeholder. This lets
  // the parent certificate preserve the structured child spelling instead
  // of rediscovering or flattening it from observed text alone.
  DenseMap<uint64_t, std::string> preferredChildSyntax;
  if (!cert.rewrittenChildSyntax.empty())
    preferredChildSyntax[cur.id] = cert.rewrittenChildSyntax;

  if (inTraceMode()) {
    // Proof-ledger logging separates the sets that are easy to conflate:
    // observed parent arguments, paste arguments required by the parent,
    // and child formals that still need a non-direct discharge.
    auto observedParentArgIdxs = collectSortedUInt32Keys(parentObserved);
    auto requiredParentPasteArgIdxs =
        collectSortedUniquePasteArgIdxs(parent->pasteSpans);
    trace("macro/proof",
          "DAG per-hop proof ledger observed: child id={0} name={1} "
          "parent id={2} name={3} observedParentArgs={4} "
          "requiredParentPasteArgs={5} unresolvedChildFormals={6}",
          cur.id, cur.name, parent->id, parent->name,
          formatUInt32List(observedParentArgIdxs),
          formatUInt32List(requiredParentPasteArgIdxs),
          formatUInt32List(unresolvedChildFormals));
  }

  // Convert the observed parent-formal constraints into concrete parent
  // formal rewrites. This is the main consistency gate for the parent
  // boundary: conflicting observations, missing structural templates, or
  // unsupported placeholder interactions all fail here.
  DenseMap<uint32_t, FormalTextPair> parentFormals;
  for (const auto &kvLocal : parentObserved) {
    const uint32_t parentFormal = kvLocal.first;
    auto formalCert =
        deps_.invertibilitySolver.BuildObservedFormalRewriteCertificate(
            *parent, parentFormal, kvLocal.second,
            preferredChildSyntax.empty() ? nullptr : &preferredChildSyntax,
            "DAG per-hop", ctx.tokenHunksAR);
    cert.parentFormalCertificates.push_back(formalCert);
    if (formalCert.kind == FormalRewriteCertificateKind::Invalid) {
      const bool templateMismatch =
          formalCert.failure == FormalRewriteFailure::MissingStructuralTemplate;
      auto srcIt = parentObservedSources.find(parentFormal);
      if (templateMismatch && srcIt != parentObservedSources.end() &&
          srcIt->second.size() == 1) {
        const uint32_t sourceCurFormal = srcIt->second.front();
        auto curIt = curFormals.find(sourceCurFormal);
        if (curIt != curFormals.end()) {
          // A single-source template mismatch may be the direct
          // passthrough case where the child observed a flattened
          // surface. Accept this recovery only when the formal dependency
          // template proves a one-to-one child-formal to parent-formal
          // mapping.
          if (auto flatten = tryBuildDirectPassthroughParentFormalRewrite(
                  sourceCurFormal, parentFormal, curIt->second.newText)) {
            parentFormals[parentFormal] = std::move(*flatten);
            continue;
          }
        }
      }
      cert.failureReason = StructuredLiftFailureReason::ParentFormalInvalid;
      cert.parentFormalFailure = formalCert.failure;
      cert.detail =
          formatv("{0}; lexical bridge required", formalCert.detail).str();
      cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
      cert.nextInv = parent;
      return cert;
    }

    parentFormals[parentFormal] =
        FormalTextPair{formalCert.oldText, formalCert.newText};
  }

  if (inTraceMode()) {
    // After formal certification, compare the parent arguments we
    // actually carry with the paste arguments that the parent may need to
    // preserve. This is diagnostic-only: the later body-space gate owns
    // the semantic discharge.
    auto observedParentArgIdxs = collectSortedUInt32Keys(parentObserved);
    auto carriedParentArgIdxs = collectSortedUInt32Keys(parentFormals);
    auto requiredParentPasteArgIdxs =
        collectSortedUniquePasteArgIdxs(parent->pasteSpans);
    auto missingParentSupportArgIdxs = computeSortedMissingUInt32s(
        requiredParentPasteArgIdxs, carriedParentArgIdxs);
    trace("macro/proof",
          "DAG per-hop proof ledger carried: child id={0} name={1} "
          "parent id={2} name={3} observedParentArgs={4} "
          "carriedParentArgs={5} requiredParentPasteArgs={6} "
          "missingSupport={7} unresolvedChildFormals={8}",
          cur.id, cur.name, parent->id, parent->name,
          formatUInt32List(observedParentArgIdxs),
          formatUInt32List(carriedParentArgIdxs),
          formatUInt32List(requiredParentPasteArgIdxs),
          formatUInt32List(missingParentSupportArgIdxs),
          formatUInt32List(unresolvedChildFormals));
  }

  // Unresolved child-formal inversions are allowed only when a parent
  // formal certificate has proven that the parent body itself preserves
  // the relevant child syntax/raw invocation input. In that case the
  // missing direct inversion is discharged by body-space semantics rather
  // than by a guessed parent-formal constraint.
  auto unresolvedDerivationsDischargedByBodySpace = [&]() {
    if (unresolvedChildFormals.empty())
      return true;
    for (const auto &formalCert : cert.parentFormalCertificates) {
      const auto &sig = formalCert.interactionConsistency.signature;
      if (sig.usesPreferredChildSyntax || sig.usesRawInvocationPreservation ||
          sig.usesRawChildInvocationLogicalInput)
        return true;
    }
    return false;
  };

  if (!unresolvedDerivationsDischargedByBodySpace()) {
    cert.failureReason =
        StructuredLiftFailureReason::ParentConstraintDerivationFailed;
    cert.derivationFailure =
        ParentConstraintDerivationFailure::InversionNotUnique;
    cert.detail = unresolvedDerivationDetails.empty()
                      ? formatv("DAG per-hop: unresolved parent "
                                "constraint derivation requires "
                                "lexical bridge: child id={0} name={1} "
                                "parent id={2} name={3}",
                                cur.id, cur.name, parent->id, parent->name)
                            .str()
                      : unresolvedDerivationDetails.front();
    cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
    cert.nextInv = parent;
    return cert;
  }

  // Final hop gate: prove that the parent invocation can be reconstructed
  // from the certified parent formal rewrites. A parent formal rewrite is
  // not enough by itself; the full invocation placeholder structure must
  // also remain valid.
  auto parentCert = this->BuildWrapperPlaceholderHopInvocationCertificate(
      ctx, *parent, parentFormals, "DAG per-hop");
  cert.parentCert = parentCert;
  if (parentCert.kind == InvocationRewriteCertificateKind::Invalid) {
    cert.failureReason = StructuredLiftFailureReason::ParentInvocationInvalid;
    cert.parentInvocationFailure = parentCert.failure;
    cert.detail = parentCert.detail;
    cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
    cert.nextInv = parent;
    return cert;
  }
  if (parentCert.kind == InvocationRewriteCertificateKind::NoChange) {
    cert.detail = formatv("DAG per-hop: parent invocation no-change child "
                          "id={0} name={1} parent id={2} name={3} "
                          "parentFormals={4}",
                          cur.id, cur.name, parent->id, parent->name,
                          parentFormals.size())
                      .str();
  } else {
    cert.detail = formatv("DAG per-hop: structured hop child id={0} name={1} "
                          "parent id={2} name={3} parentFormals={4}",
                          cur.id, cur.name, parent->id, parent->name,
                          parentFormals.size())
                      .str();
  }

  // The hop is now fully certified: the next outer invocation is the
  // parent, and `nextFormals` carries the rewritten parent formals for
  // the next structured lift step.
  cert.kind = StructuredLiftCertificateKind::Unique;
  cert.nextInv = parent;
  cert.nextFormals = std::move(parentFormals);
  cert.parentFormalSources = std::move(parentObservedSources);
  return cert;
}

LiftChainCertificate RefoldMacroDAGStructuredLifter::BuildLiftChainCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const RefoldModel::MacroInvocation &leaf,
    const DenseMap<uint32_t, FormalTextPair> &leafFormals) const {
  LiftChainCertificate cert;
  cert.leaf = &leaf;

  // Normalize the starting point of the chain to only the leaf formals
  // that actually changed. No-change formals do not need to be lifted and
  // would only add noise to later proof obligations.
  for (const auto &kvLocal : leafFormals) {
    StringRef oldText = StringRef(kvLocal.second.oldText).trim();
    StringRef newText = StringRef(kvLocal.second.newText).trim();
    if (oldText == newText)
      continue;
    cert.leafArgIdxs.push_back(kvLocal.first);
    cert.leafFormals[kvLocal.first] =
        FormalTextPair{oldText.str(), newText.str()};
  }
  llvm::sort(cert.leafArgIdxs);

  // Nothing distinct reached the leaf boundary, so there is no lift chain
  // to build. This is not a proof failure; it is simply a non-candidate.
  if (cert.leafFormals.empty()) {
    cert.detail = formatv("DAG lift chain: leaf id={0} name={1} has no "
                          "distinct leaf formals to lift",
                          leaf.id, leaf.name)
                      .str();
    return cert;
  }

  const RefoldModel::MacroInvocation *cur = &leaf;
  DenseMap<uint32_t, FormalTextPair> curFormals;
  DenseSet<uint32_t> bridgedCurFormals;
  curFormals = cert.leafFormals;

  // Walk from the edited leaf toward the selected root invocation `m`.
  // At each point, `curFormals` is the certified rewrite at the current
  // invocation boundary.
  while (cur->id != ctx.m.id) {
    auto step = this->BuildStructuredLiftCertificate(ctx, *cur, curFormals);
    cert.steps.push_back(step);

    // A hard invalid step means the hop failed before producing any
    // usable parent boundary. There is no safe bridge target to continue
    // from in this case.
    if (step.kind == StructuredLiftCertificateKind::Invalid) {
      cert.detail = step.detail;
      return cert;
    }

    if (step.kind == StructuredLiftCertificateKind::Unique) {
      if (!step.nextInv) {
        cert.detail = formatv("DAG lift chain: missing next invocation "
                              "after structured hop child id={0} name={1}",
                              cur->id, cur->name)
                          .str();
        return cert;
      }

      // Propagate bridge provenance through a successful structured hop.
      // If a current formal was bridge-derived, then any parent formal
      // whose certificate depends on that current formal is also marked
      // bridge-derived.
      DenseSet<uint32_t> nextBridgedCurFormals;
      if (!bridgedCurFormals.empty()) {
        for (const auto &kvLocal : step.nextFormals) {
          auto srcIt = step.parentFormalSources.find(kvLocal.first);
          if (srcIt == step.parentFormalSources.end())
            continue;
          for (uint32_t sourceCurFormal : srcIt->second) {
            if (bridgedCurFormals.contains(sourceCurFormal)) {
              nextBridgedCurFormals.insert(kvLocal.first);
              break;
            }
          }
        }
      }

      // Advance to the parent using the structured proof result.
      cert.steps.back().bridgedNextFormals = nextBridgedCurFormals;
      cur = step.nextInv;
      curFormals = std::move(step.nextFormals);
      bridgedCurFormals = std::move(nextBridgedCurFormals);
      continue;
    }

    // The structured hop could not be proven, so the step must have
    // supplied both a parent invocation and a certified rewritten child
    // spelling for the lexical bridge path.
    if (!step.nextInv || step.rewrittenChildSyntax.empty()) {
      cert.detail = formatv("DAG lift chain: lexical bridge unavailable "
                            "child id={0} name={1}",
                            cur->id, cur->name)
                        .str();
      return cert;
    }

    // Bridge by replacing the child occurrence in the parent with the
    // already-certified rewritten child syntax. The bridge must return a
    // concrete parent-formal rewrite map; otherwise the lift cannot
    // continue soundly.
    auto bridged = this->TryLexicalChildBridge(ctx, *step.nextInv, *cur,
                                               step.rewrittenChildSyntax);
    if (!bridged) {
      cert.detail =
          formatv("DAG lift chain: lexical bridge failed "
                  "parent id={0} name={1} child id={2} name={3}",
                  step.nextInv->id, step.nextInv->name, cur->id, cur->name)
              .str();
      return cert;
    }

    // After a lexical bridge, every produced parent formal is marked as
    // bridge-derived. Later structured hops may propagate that provenance
    // farther outward through their parent-formal source maps.
    cert.usedLexicalBridge = true;
    cur = step.nextInv;
    curFormals = std::move(*bridged);
    bridgedCurFormals.clear();
    for (const auto &kvLocal : curFormals)
      bridgedCurFormals.insert(kvLocal.first);
    cert.steps.back().bridgedNextFormals = bridgedCurFormals;
  }

  // We have reached the root invocation. The current formal rewrite map
  // is now the root-formal rewrite map for the complete lift chain.
  for (const auto &kvLocal : curFormals) {
    cert.rootFormals[kvLocal.first] = kvLocal.second;
    if (bridgedCurFormals.contains(kvLocal.first))
      cert.bridgedRootArgIdxs.insert(kvLocal.first);
  }

  cert.kind = LiftChainCertificateKind::Unique;
  cert.detail = formatv("DAG lift chain: leaf id={0} name={1} leafArgs={2} "
                        "steps={3} lexicalBridge={4} rootFormals={5}",
                        leaf.id, leaf.name, formatUInt32List(cert.leafArgIdxs),
                        cert.steps.size(), cert.usedLexicalBridge ? 1 : 0,
                        cert.rootFormals.size())
                    .str();
  return cert;
}

RootFormalMergeCertificate
RefoldMacroDAGStructuredLifter::BuildRootFormalMergeCertificate(
    const RefoldMacroDAGLiftingContext &ctx, uint32_t argIdx,
    ArrayRef<FormalTextPair> rewrites, StringRef traceStage) const {
  RootFormalMergeCertificate cert;
  cert.argIdx = argIdx;
  cert.observedRewrites.assign(rewrites.begin(), rewrites.end());

  // The merge is defined only for formals that exist on the root
  // invocation. Reject stale or malformed dependency information before
  // slicing the invocation text.
  if (argIdx >= ctx.invArgRanges.size()) {
    cert.failure = RootFormalMergeFailure::ArgIndexOutOfBounds;
    cert.detail = formatv("{0}: root arg index out of bounds root id={1} "
                          "name={2} argIdx={3} ctx.invArgRanges.size()={4}",
                          traceStage, ctx.m.id, ctx.m.name, argIdx,
                          ctx.invArgRanges.size())
                      .str();
    return cert;
  }

  const size_t begin = ctx.invArgRanges[argIdx].first;
  const size_t end = ctx.invArgRanges[argIdx].second;
  if (begin > end || end > ctx.invSpanText.size()) {
    cert.failure = RootFormalMergeFailure::InvalidArgRange;
    cert.detail = formatv("{0}: root arg range invalid root id={1} name={2} "
                          "argIdx={3} range=[{4},{5}) spanLen={6}",
                          traceStage, ctx.m.id, ctx.m.name, argIdx, begin, end,
                          ctx.invSpanText.size())
                      .str();
    return cert;
  }

  const StringRef baseArgText = ctx.invSpanText.slice(begin, end).trim();
  cert.baseArgText = baseArgText.str();

  // This is the only semantic merge point for root-formal rewrites. The
  // helper must prove the rewrite set is compatible; otherwise competing
  // leaf chains are not allowed to silently overwrite one another.
  auto mergedNewArg = deps_.invertibilitySolver.MergeCompatibleFormalRewrites(
      baseArgText, rewrites);
  if (!mergedNewArg) {
    cert.failure = RootFormalMergeFailure::MergeConflict;
    cert.detail = formatv("{0}: root rewrite merge conflicted root id={1} "
                          "name={2} argIdx={3}",
                          traceStage, ctx.m.id, ctx.m.name, argIdx)
                      .str();
    return cert;
  }

  cert.mergedArgText = StringRef(*mergedNewArg).trim().str();
  cert.kind = cert.baseArgText == cert.mergedArgText
                  ? RootFormalMergeCertificateKind::NoChange
                  : RootFormalMergeCertificateKind::Unique;
  return cert;
}

} // namespace refold
} // namespace clang
