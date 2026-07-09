//===--- RefoldMacroDAGCandidateValidator.cpp -------------------*- C++ -*-===//
//
// Final DAG candidate validation service.
//
// This translation unit owns the root-candidate acceptance checks that combine
// structured lifting, subtree certificates, paste discharge, and root-patch
// construction metadata.  The implementation is intentionally context-driven:
// `RefoldMacroDAGLiftingContext` carries immutable per-run inputs, while
// `DagCandidateAcceptanceContext` carries the mutable uniqueness/conflict state
// for the candidate set being validated.  Sibling DAG services are reached
// through Dependencies, keeping validation policy local to the macro DAG
// subsystem.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDAGCandidateValidator.h"

#include "core/RefoldLog.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroDAGCandidateValidator::RefoldMacroDAGCandidateValidator(
    Dependencies deps)
    : deps_(std::move(deps)) {}

std::string
RefoldMacroDAGCandidateValidator::FormatBridgeSensitiveFormalSignatureMap(
    const StringMap<SemanticInteractionSignature> &sigs) const {
  SmallVector<StringRef, 8> keys;
  keys.reserve(sigs.size());
  for (const auto &kvLocal : sigs)
    keys.push_back(kvLocal.getKey());
  llvm::sort(keys);

  std::string out;
  raw_string_ostream os(out);
  os << "{";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i)
      os << ", ";
    StringRef key = keys[i];
    const auto it = sigs.find(key);
    os << key << ":(paste=" << (it->second.touchesPaste ? 1 : 0)
       << ", stringify=" << (it->second.usesStringify ? 1 : 0)
       << ", wide=" << (it->second.usesWideStringify ? 1 : 0)
       << ", wrapper=" << (it->second.usesPassthroughFlatten ? 1 : 0)
       << ", childSyntax=" << (it->second.usesPreferredChildSyntax ? 1 : 0)
       << ", raw=" << (it->second.usesRawInvocationPreservation ? 1 : 0) << ")";
  }
  os << "}";
  return os.str();
}

RootPatchConstructionCertificate
RefoldMacroDAGCandidateValidator::BuildRootPatchConstructionCertificate(
    const RefoldMacroDAGLiftingContext &ctx,
    const InvocationRewriteCertificate &rootCert, StringRef traceStage) const {
  RootPatchConstructionCertificate cert;

  // A root certificate with no effective rewrites does not need a patch.
  if (rootCert.kind == InvocationRewriteCertificateKind::NoChange ||
      rootCert.rewrites.empty()) {
    cert.kind = RootPatchConstructionCertificateKind::NoChange;
    cert.detail =
        formatv("{0}: root patch construction no-op root id={1} "
                "name={2}",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
            .str();
    return cert;
  }

  // Convert each certified root-formal rewrite into an argument-local
  // byte edit within the root invocation text. Range validation happens
  // here because this is the first point where semantic formal indexes
  // become concrete source slices.
  cert.edits.reserve(rootCert.rewrites.size());
  for (const auto &rewrite : rootCert.rewrites) {
    const uint32_t argIdx = rewrite.argIdx;
    if (argIdx >= ctx.rootInvocationArgRanges.size()) {
      cert.failure = RootPatchConstructionFailure::ArgIndexOutOfBounds;
      cert.detail =
          formatv("{0}: root patch construction failed root id={1} "
                  "name={2} argIdx={3} out of bounds argCount={4}",
                  traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                  argIdx, ctx.rootInvocationArgRanges.size())
              .str();
      return cert;
    }

    const uint64_t begin = (uint64_t)ctx.rootInvocationArgRanges[argIdx].first;
    const uint64_t end = (uint64_t)ctx.rootInvocationArgRanges[argIdx].second;
    if (begin > end || end > (uint64_t)ctx.rootInvocationText.size()) {
      cert.failure = RootPatchConstructionFailure::InvalidArgRange;
      cert.detail =
          formatv("{0}: root patch construction failed root id={1} "
                  "name={2} argIdx={3} invalid range=[{4},{5}) "
                  "spanLen={6}",
                  traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                  argIdx, begin, end, ctx.rootInvocationText.size())
              .str();
      return cert;
    }

    cert.edits.push_back(ArgEdit{begin, end, rewrite.newText});
  }

  // Apply edits in source order and reject overlap. Root formal argument
  // ranges should be disjoint; overlap here indicates malformed range
  // metadata or an invalid patch-construction request.
  llvm::sort(cert.edits, [](const ArgEdit &a, const ArgEdit &b) {
    return a.begin < b.begin;
  });

  uint64_t cur = 0;
  for (const auto &e : cert.edits) {
    if (e.begin < cur || e.end < e.begin) {
      cert.failure = RootPatchConstructionFailure::OverlappingEdits;
      cert.detail = formatv("{0}: root patch construction failed root id={1} "
                            "name={2} overlapping edits range=[{3},{4}) "
                            "prevEnd={5}",
                            traceStage, ctx.rootInvocation.id,
                            ctx.rootInvocation.name, e.begin, e.end, cur)
                        .str();
      return cert;
    }
    cur = e.end;
  }

  // Materialize the replacement for the entire root invocation by copying
  // untouched text between argument edits and substituting each certified
  // new argument spelling at its original argument range. At the same time,
  // remember the output-side subrange occupied by the substituted root
  // argument payloads. The physical patch still covers the full invocation,
  // but the optional edit map should point at the root formal text that
  // actually represents the B-side materialized hunk.
  std::string replText;
  replText.reserve(ctx.rootInvocationText.size());
  std::optional<uint64_t> materializedOutputBegin;
  std::optional<uint64_t> materializedOutputEnd;
  cur = 0;
  for (const auto &e : cert.edits) {
    auto mid = ctx.rootInvocationText.slice((size_t)cur, (size_t)e.begin);
    replText.append(mid.begin(), mid.end());
    const uint64_t replBegin = static_cast<uint64_t>(replText.size());
    replText.append(e.repl);
    const uint64_t replEnd = static_cast<uint64_t>(replText.size());
    materializedOutputBegin =
        materializedOutputBegin ? std::min(*materializedOutputBegin, replBegin)
                                : replBegin;
    materializedOutputEnd = materializedOutputEnd
                                ? std::max(*materializedOutputEnd, replEnd)
                                : replEnd;
    cur = e.end;
  }
  auto tail = ctx.rootInvocationText.drop_front((size_t)cur);
  replText.append(tail.begin(), tail.end());

  MacroPatch patch{ctx.invStart, ctx.invEnd, std::move(replText), 0};
  if (materializedOutputBegin && materializedOutputEnd) {
    patch.materialized.hasOutputByteRange = true;
    patch.materialized.outputByteStart = *materializedOutputBegin;
    patch.materialized.outputByteEnd = *materializedOutputEnd;
  }

  // Target-PP identity for a DAG root replay is the union of the exact
  // B-token envelopes for the root formal occurrences that the accepted
  // certificate rewrites.  This is deliberately derived from the
  // producer-recorded PPArgSpan -> B-token mapping, not from the source
  // replacement text, so the resolver can distinguish proof of output
  // identity from a spelling preview.
  std::optional<std::pair<uint64_t, uint64_t>> materializedBTokenRange;
  for (const auto &rewrite : rootCert.rewrites) {
    for (const auto &span : ctx.rootInvocation.argSpans) {
      if (span.kind != PPArgSpanKind::Standard ||
          span.argIdx != rewrite.argIdx || span.begin >= span.end)
        continue;

      std::optional<std::pair<size_t, size_t>> bEnv =
          deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(span);
      if (!bEnv || bEnv->second <= bEnv->first ||
          bEnv->second > deps_.bToks.size())
        continue;

      const uint64_t bBegin = static_cast<uint64_t>(bEnv->first);
      const uint64_t bEnd = static_cast<uint64_t>(bEnv->second);
      if (!materializedBTokenRange) {
        materializedBTokenRange = std::make_pair(bBegin, bEnd);
      } else {
        materializedBTokenRange->first =
            std::min(materializedBTokenRange->first, bBegin);
        materializedBTokenRange->second =
            std::max(materializedBTokenRange->second, bEnd);
      }
    }
  }
  if (materializedBTokenRange) {
    patch.materialized.hasBTokenRange = true;
    patch.materialized.bTokStart = materializedBTokenRange->first;
    patch.materialized.bTokEnd = materializedBTokenRange->second;
  } else if (std::optional<WholeCoverPlan> plan =
                 deps_.computeWholeCoverPlan(ctx.m)) {
    // A DAG subtree-root repair may rewrite a root actual whose
    // final B-side tokens are produced only by descendant generated
    // callee/stringify/paste expansions.  Such roots have no direct
    // Standard PPArgSpan -> B-token envelope for the rewritten formal,
    // but the already-validated subtree/root replay proves that the
    // preserved root invocation materializes this owner-closed macro
    // expansion.  Use the producer-derived whole-cover B envelope as
    // target-PP proof; never derive target identity from replacement
    // source text.
    patch.materialized.hasBTokenRange = true;
    patch.materialized.bTokStart = static_cast<uint64_t>(plan->bTokStart);
    patch.materialized.bTokEnd = static_cast<uint64_t>(plan->bTokEnd);
  }

  cert.patch = std::move(patch);
  cert.kind = RootPatchConstructionCertificateKind::Unique;
  cert.detail = formatv("{0}: root patch construction succeeded root id={1} "
                        "name={2} edits={3} replLen={4}",
                        traceStage, ctx.rootInvocation.id,
                        ctx.rootInvocation.name, cert.edits.size(),
                        cert.patch ? cert.patch->replacement.size() : 0)
                    .str();
  return cert;
}

std::optional<DagCandidateValidationMetadata>
RefoldMacroDAGCandidateValidator::MergeDagCandidateValidationMetadata(
    const RefoldMacroDAGLiftingContext &ctx,
    const DagCandidateValidationMetadata &lhs,
    const DagCandidateValidationMetadata &rhs) const {
  DagCandidateValidationMetadata merged;

  // Boolean hazards compose by union: if either side observed a semantic
  // condition that requires later validation, the merged candidate must
  // carry that condition forward.
  merged.hasBridgeSensitiveStructuredSemantics =
      lhs.hasBridgeSensitiveStructuredSemantics ||
      rhs.hasBridgeSensitiveStructuredSemantics;
  merged.hasMixedSemanticInteractions =
      lhs.hasMixedSemanticInteractions || rhs.hasMixedSemanticInteractions;

  // Bridge-sensitive formal signatures are keyed by logical formal. The
  // same formal may appear in both candidates, but only with identical
  // semantic evidence; divergent signatures mean the candidates cannot be
  // soundly composed.
  for (const auto &kvLocal : lhs.bridgeSensitiveFormalSignatures)
    merged.bridgeSensitiveFormalSignatures[kvLocal.getKey()] =
        kvLocal.getValue();
  for (const auto &kvLocal : rhs.bridgeSensitiveFormalSignatures) {
    auto it = merged.bridgeSensitiveFormalSignatures.find(kvLocal.getKey());
    if (it == merged.bridgeSensitiveFormalSignatures.end()) {
      merged.bridgeSensitiveFormalSignatures[kvLocal.getKey()] =
          kvLocal.getValue();
      continue;
    }
    if (!(it->second == kvLocal.getValue()))
      return std::nullopt;
  }

  // Deferred occurrence arguments also compose by set union. Keep the
  // resulting vector sorted so downstream diagnostics and comparisons are
  // deterministic.
  auto addDeferredArgIdxs = [&](ArrayRef<uint32_t> argIdxs) {
    for (uint32_t argIdx : argIdxs) {
      if (!llvm::is_contained(merged.deferOccurrenceArgIdxs, argIdx))
        merged.deferOccurrenceArgIdxs.push_back(argIdx);
    }
  };
  addDeferredArgIdxs(lhs.deferOccurrenceArgIdxs);
  addDeferredArgIdxs(rhs.deferOccurrenceArgIdxs);
  llvm::sort(merged.deferOccurrenceArgIdxs);

  if (!lhs.hasExpectedRootFormals && !rhs.hasExpectedRootFormals)
    return merged;

  // Collect expected root-formal rewrites from both candidates by root
  // argument. Compatibility is checked per argument against the original
  // root spelling below.
  DenseMap<uint32_t, SmallVector<FormalTextPair, 2>> rewritesByArg;
  auto collect = [&](const DagCandidateValidationMetadata &meta) {
    if (!meta.hasExpectedRootFormals)
      return;
    for (const auto &kvLocal : meta.expectedRootFormals)
      rewritesByArg[kvLocal.first].push_back(kvLocal.second);
  };
  collect(lhs);
  collect(rhs);

  for (const auto &kvLocal : rewritesByArg) {
    const uint32_t argIdx = kvLocal.first;
    if (argIdx >= ctx.rootInvocationArgRanges.size())
      return std::nullopt;

    const size_t begin = ctx.rootInvocationArgRanges[argIdx].first;
    const size_t end = ctx.rootInvocationArgRanges[argIdx].second;
    if (begin > end || end > ctx.rootInvocationText.size())
      return std::nullopt;

    // The merge is anchored to the original root argument text. This
    // prevents two candidates from overwriting each other unless their
    // proposed rewrites are mutually compatible with the same base text.
    const StringRef baseArgText =
        ctx.rootInvocationText.slice(begin, end).trim();
    auto mergedArgText =
        deps_.invertibilitySolver.MergeCompatibleFormalRewrites(baseArgText,
                                                                kvLocal.second);
    if (!mergedArgText)
      return std::nullopt;
    if (StringRef(*mergedArgText).trim() == baseArgText) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "merge DAG validation metadata root id={0} name={1} "
          "argIdx={2} collapsed to base text base='{3}' variants={4}",
          ctx.rootInvocation.id, ctx.rootInvocation.name, argIdx,
          stringutils::showWsWithClip(baseArgText, 120), kvLocal.second.size());
      continue;
    }

    merged.expectedRootFormals[argIdx] = FormalTextPair{
        baseArgText.str(), StringRef(*mergedArgText).trim().str()};
  }

  merged.hasExpectedRootFormals = true;
  return merged;
}

std::optional<InvocationHeadShape>
RefoldMacroDAGCandidateValidator::GetInvocationHeadShape(
    const RefoldMacroDAGLiftingContext &ctx, StringRef text) const {
  StringRef trimmed = text.trim();
  auto argRangesOpt =
      RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
          trimmed, deps_.lexLang);
  if (!argRangesOpt)
    return std::nullopt;

  size_t open = trimmed.find('(');
  if (open == StringRef::npos)
    return std::nullopt;

  StringRef callee = trimmed.take_front(open).trim();
  if (callee.empty())
    return std::nullopt;

  return InvocationHeadShape{callee.str(), argRangesOpt->size()};
}

bool RefoldMacroDAGCandidateValidator::PreservesRootInvocationHead(
    const RefoldMacroDAGLiftingContext &ctx,
    const FormalTextPair &rewrite) const {
  auto oldShape = this->GetInvocationHeadShape(ctx, rewrite.oldText);
  auto newShape = this->GetInvocationHeadShape(ctx, rewrite.newText);
  if (!oldShape || !newShape)
    return false;
  return oldShape->callee == newShape->callee &&
         oldShape->argCount == newShape->argCount;
}

unsigned RefoldMacroDAGCandidateValidator::CountPreservedInvocationHeads(
    const RefoldMacroDAGLiftingContext &ctx, StringRef oldText,
    StringRef newText) const {
  auto oldShape = this->GetInvocationHeadShape(ctx, oldText);
  auto newShape = this->GetInvocationHeadShape(ctx, newText);
  if (!oldShape || !newShape)
    return 0;
  if (oldShape->callee != newShape->callee ||
      oldShape->argCount != newShape->argCount)
    return 0;

  auto oldArgRangesOpt =
      RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
          oldText.trim(), deps_.lexLang);
  auto newArgRangesOpt =
      RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
          newText.trim(), deps_.lexLang);
  if (!oldArgRangesOpt || !newArgRangesOpt ||
      oldArgRangesOpt->size() != newArgRangesOpt->size())
    return 0;

  // The outer invocation head is preserved. Recurse positionally through
  // corresponding argument slices to count any nested invocation heads
  // that are also preserved by the rewrite.
  unsigned score = 1;
  for (size_t i = 0; i < oldArgRangesOpt->size(); ++i) {
    const auto &oldArgRange = (*oldArgRangesOpt)[i];
    const auto &newArgRange = (*newArgRangesOpt)[i];
    score += this->CountPreservedInvocationHeads(
        ctx,
        oldText.trim().slice((size_t)oldArgRange.first,
                             (size_t)oldArgRange.second),
        newText.trim().slice((size_t)newArgRange.first,
                             (size_t)newArgRange.second));
  }
  return score;
}

int RefoldMacroDAGCandidateValidator::ChoosePreferredStructuredDagCandidate(
    const RefoldMacroDAGLiftingContext &ctx,
    const DagCandidateValidationMetadata &existingValidation,
    const DagCandidateValidationMetadata &candidateValidation) const {
  if (!existingValidation.hasExpectedRootFormals ||
      !candidateValidation.hasExpectedRootFormals)
    return 0;

  if (existingValidation.expectedRootFormals.size() !=
      candidateValidation.expectedRootFormals.size())
    return 0;

  bool existingPreferred = false;
  bool candidatePreferred = false;

  // Compare candidates formal-by-formal. The comparison is only valid if
  // both candidates cover exactly the same root formals and start from
  // the same base argument text for each formal.
  for (const auto &kvLocal : existingValidation.expectedRootFormals) {
    auto it = candidateValidation.expectedRootFormals.find(kvLocal.first);
    if (it == candidateValidation.expectedRootFormals.end())
      return 0;

    const FormalTextPair &existingRewrite = kvLocal.second;
    const FormalTextPair &candidateRewrite = it->second;
    if (StringRef(existingRewrite.oldText).trim() !=
        StringRef(candidateRewrite.oldText).trim())
      return 0;

    const bool existingPreserves =
        this->PreservesRootInvocationHead(ctx, existingRewrite);
    const bool candidatePreserves =
        this->PreservesRootInvocationHead(ctx, candidateRewrite);
    const unsigned existingStructureScore = this->CountPreservedInvocationHeads(
        ctx, existingRewrite.oldText, existingRewrite.newText);
    const unsigned candidateStructureScore =
        this->CountPreservedInvocationHeads(ctx, candidateRewrite.oldText,
                                            candidateRewrite.newText);

    // Prefer the candidate that preserves more nested invocation heads.
    // This gives deeper structure preservation priority over the weaker
    // outer-head-only predicate below.
    if (existingStructureScore != candidateStructureScore) {
      if (existingStructureScore > candidateStructureScore)
        existingPreferred = true;
      if (candidateStructureScore > existingStructureScore)
        candidatePreferred = true;
      continue;
    }

    if (existingPreserves == candidatePreserves) {
      if (StringRef(existingRewrite.newText).trim() !=
          StringRef(candidateRewrite.newText).trim())
        return 0;
      continue;
    }

    // If the recursive score ties, use outer root-invocation-head
    // preservation as the final structural preference signal.
    if (existingPreserves)
      existingPreferred = true;
    if (candidatePreserves)
      candidatePreferred = true;
  }

  if (existingPreferred == candidatePreferred)
    return 0;
  return existingPreferred ? -1 : 1;
}

bool RefoldMacroDAGCandidateValidator::
    RootDeferredPasteReplayHasOnlyProvenDependentUses(
        const RefoldMacroDAGLiftingContext &ctx,
        const DenseMap<uint32_t, FormalTextPair> &rootFormals,
        StringRef traceStage, std::string &failureDetail) const {
  struct DependentUse {
    const RefoldModel::MacroInvocation *inv = nullptr;
    uint32_t argIdx = 0;
    const char *kind = "";
    uint64_t begin = 0;
    uint64_t end = 0;
    bool hiddenPasteSelector = false;
  };

  auto tokenRangeTouchesCurrentDiff = [&](uint64_t begin,
                                          uint64_t end) -> bool {
    if (begin >= end)
      return false;
    for (const auto &h : ctx.tokenHunksAR) {
      if (h.aStart == h.aEnd) {
        // Pure insertions belong to the adjacent dependent surface when
        // the insertion point is exactly inside or on either boundary.
        if (h.aStart >= begin && h.aStart <= end)
          return true;
        continue;
      }
      if (h.aStart < end && h.aEnd > begin)
        return true;
    }
    return false;
  };

  auto addArgSpanUses = [&](SmallVectorImpl<DependentUse> &uses,
                            const RefoldModel::MacroInvocation &inv,
                            uint32_t argIdx, const char *kind,
                            ArrayRef<RefoldModel::PPArgSpan> spans) {
    for (const auto &sp : spans) {
      if (sp.argIdx != argIdx || !sp.IsValid())
        continue;
      uses.push_back(DependentUse{&inv, argIdx, kind, sp.begin, sp.end, false});
    }
  };

  auto pasteTokenUsesFormal = [&](const RefoldModel::MacroInvocation &inv,
                                  uint32_t argIdx) {
    for (const auto &tok : inv.pasteTokens) {
      for (const auto &part : tok.parts) {
        if (part.kind == RefoldModel::PastePartKind::Arg && part.argIndex &&
            *part.argIndex == argIdx)
          return true;
      }
    }
    return false;
  };

  auto hasFinalPasteSpanForRange = [&](const RefoldModel::MacroInvocation &inv,
                                       uint32_t argIdx, uint64_t begin,
                                       uint64_t end) {
    for (const auto &ps : inv.pasteSpans) {
      if (ps.argIdx == argIdx && ps.begin == begin && ps.end == end)
        return true;
    }
    return false;
  };

  auto addPasteTokenUses = [&](SmallVectorImpl<DependentUse> &uses,
                               const RefoldModel::MacroInvocation &inv,
                               uint32_t argIdx) {
    auto addOne = [&](const RefoldModel::PPSpan &sp) {
      if (!sp.IsValid())
        return;
      const bool hasFinalPasteSpan =
          hasFinalPasteSpanForRange(inv, argIdx, sp.begin, sp.end);
      uses.push_back(DependentUse{&inv, argIdx,
                                  hasFinalPasteSpan ? "paste-token"
                                                    : "hidden-paste-selector",
                                  sp.begin, sp.end, !hasFinalPasteSpan});
    };

    if (!inv.spans.empty()) {
      for (const auto &sp : inv.spans)
        addOne(sp);
    } else {
      for (const auto &sp : inv.bodySpans)
        addOne(sp);
    }
  };

  auto formatDependentUse = [&](const DependentUse &use) {
    return formatv("{{id={0},name={1},arg={2},kind={3},A=[{4},{5}),"
                   "touched={6},hiddenSelector={7}}}",
                   use.inv ? use.inv->id : 0,
                   use.inv ? use.inv->name : StringRef(""), use.argIdx,
                   use.kind, use.begin, use.end,
                   tokenRangeTouchesCurrentDiff(use.begin, use.end) ? 1 : 0,
                   use.hiddenPasteSelector ? 1 : 0)
        .str();
  };

  for (const auto &kvLocal : rootFormals) {
    StringRef oldText = StringRef(kvLocal.second.oldText).trim();
    StringRef newText = StringRef(kvLocal.second.newText).trim();
    if (oldText == newText)
      continue;

    SmallVector<DependentUse, 32> uses;
    std::set<std::pair<uint64_t, uint32_t>> visited;
    SmallVector<std::pair<const RefoldModel::MacroInvocation *, uint32_t>, 32>
        work;
    work.push_back({&ctx.m, kvLocal.first});

    while (!work.empty()) {
      auto [cur, curFormal] = work.pop_back_val();
      if (!cur)
        continue;
      if (!visited.insert({cur->id, curFormal}).second)
        continue;

      // Direct final-output occurrences of this formal must be covered by
      // the same diff that justified the root rewrite.
      addArgSpanUses(uses, *cur, curFormal, "standard", cur->argSpans);
      addArgSpanUses(uses, *cur, curFormal, "stringify", cur->stringifySpans);
      addArgSpanUses(uses, *cur, curFormal, "paste-span", cur->pasteSpans);

      if (pasteTokenUsesFormal(*cur, curFormal)) {
        // A paste token may be consumed immediately as a macro selector.
        // Such hidden selectors have no final paste span, so replaying a
        // different root formal would select a macro body that this map did
        // not observe.  Record them distinctly and reject below rather
        // than pretending the wrapper replay proved their expansion.
        addPasteTokenUses(uses, *cur, curFormal);
      }

      ArrayRef<const RefoldModel::MacroInvocation *> children =
          deps_.macroTopology.MacroChildrenOf(cur->id);
      if (children.empty())
        continue;

      for (const auto *child : children) {
        if (!child)
          continue;
        for (uint32_t childFormal = 0; childFormal < child->argDeps.size();
             ++childFormal) {
          if (llvm::is_contained(child->argDeps[childFormal], curFormal))
            work.push_back({child, childFormal});
        }
      }
    }

    SmallVector<std::string, 16> rejectedUses;
    for (const auto &use : uses) {
      if (use.hiddenPasteSelector ||
          !tokenRangeTouchesCurrentDiff(use.begin, use.end))
        rejectedUses.push_back(formatDependentUse(use));
    }

    if (inTraceMode()) {
      SmallVector<std::string, 16> formattedUses;
      formattedUses.reserve(uses.size());
      for (const auto &use : uses)
        formattedUses.push_back(formatDependentUse(use));
      trace("macro/proof",
            "{0}: root deferred paste replay dependent-use coverage "
            "root id={1} name={2} argIdx={3} old='{4}' new='{5}' "
            "uses={6} rejected={7}",
            traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
            kvLocal.first, stringutils::showWsWithClip(oldText, 120),
            stringutils::showWsWithClip(newText, 120),
            llvm::join(formattedUses, ", "), llvm::join(rejectedUses, ", "));
    }

    if (!rejectedUses.empty()) {
      failureDetail =
          formatv("{0}: root proof validation rejected deferred paste "
                  "wrapper replay root id={1} name='{2}' argIdx={3} "
                  "because changed root formal has unproven "
                  "dependent output uses: {4}",
                  traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                  kvLocal.first, llvm::join(rejectedUses, ", "))
              .str();
      return false;
    }
  }

  return true;
}

RootProofValidationCertificate
RefoldMacroDAGCandidateValidator::BuildRootProofValidationCertificate(
    const RefoldMacroDAGLiftingContext &ctx, StringRef baseText,
    StringRef newText, ArrayRef<uint32_t> deferOccurrenceArgIdxs,
    const DenseMap<uint32_t, FormalTextPair> *expectedRootFormals,
    StringRef traceStage) const {
  RootProofValidationCertificate cert;

  // Identical root text needs no replay proof beyond the no-op witness.
  if (baseText == newText) {
    cert.valid = true;
    cert.detail =
        formatv("{0}: root proof validation no-op root id={1} "
                "name='{2}'",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
            .str();
    return cert;
  }

  // Recover the formal rewrite map implied by the concrete callsite text.
  // If this fails, the replacement cannot be tied back to root arguments.
  auto replayRootFormals =
      deps_.structuredLifter.BuildRootFormalRewriteMapFromCallsiteReplacement(
          ctx, baseText, newText);
  if (!replayRootFormals) {
    cert.detail =
        formatv("{0}: root proof validation failed root id={1} "
                "name='{2}' could not derive replay root-formal "
                "rewrite map baseLen={3} newLen={4}",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                baseText.size(), newText.size())
            .str();
    return cert;
  }

  if (expectedRootFormals) {
    // Some expected formals are support-only no-change entries used to
    // preserve proof context. They may not appear in the replay-derived
    // changed-formal map, so allow them only when the concrete old/new
    // argument text is unchanged and exactly matches the expected pair.
    auto concreteArgMatchesExpectedUnchanged =
        [&](uint32_t argIdx, const FormalTextPair &expected) -> bool {
      if (argIdx >= ctx.rootInvocationArgRanges.size())
        return false;

      auto newRangesOpt =
          deps_.getMacroInvocationFormalArgContentRanges(ctx.m, newText);
      if (!newRangesOpt || argIdx >= newRangesOpt->size())
        return false;

      const auto &oldR = ctx.rootInvocationArgRanges[argIdx];
      const auto &newR = (*newRangesOpt)[argIdx];
      if (oldR.first > oldR.second || oldR.second > baseText.size() ||
          newR.first > newR.second || newR.second > newText.size())
        return false;

      StringRef concreteOld =
          baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
      StringRef concreteNew =
          newText.slice((size_t)newR.first, (size_t)newR.second).trim();
      StringRef expectedOld = StringRef(expected.oldText).trim();
      StringRef expectedNew = StringRef(expected.newText).trim();
      return concreteOld == concreteNew && concreteOld == expectedOld &&
             concreteNew == expectedNew;
    };

    SmallVector<uint32_t, 8> replayAugmentedSupportOnlyArgIdxs;
    const bool traceRootProofValidation = inTraceMode();
    for (const auto &kvLocal : *expectedRootFormals) {
      if (replayRootFormals->contains(kvLocal.first))
        continue;
      if (StringRef(kvLocal.second.oldText).trim() !=
          StringRef(kvLocal.second.newText).trim())
        continue;
      if (!concreteArgMatchesExpectedUnchanged(kvLocal.first, kvLocal.second))
        continue;
      (*replayRootFormals)[kvLocal.first] = kvLocal.second;
      if (traceRootProofValidation)
        replayAugmentedSupportOnlyArgIdxs.push_back(kvLocal.first);
    }

    if (traceRootProofValidation) {
      llvm::sort(replayAugmentedSupportOnlyArgIdxs);
      trace(
          "macro/proof",
          "{0}: root proof replay-vs-expected root id={1} name={2} "
          "replay={3} expected={4} augmentedSupportOnly={5}",
          traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
          deps_.invertibilitySolver.FormatFormalTextPairMap(*replayRootFormals),
          deps_.invertibilitySolver.FormatFormalTextPairMap(
              *expectedRootFormals),
          formatUInt32List(replayAugmentedSupportOnlyArgIdxs));

      auto newRangesOpt =
          deps_.getMacroInvocationFormalArgContentRanges(ctx.m, newText);
      SmallVector<uint32_t, 8> missingExpectedArgIdxs;
      SmallVector<uint32_t, 8> unchangedConcreteMissingArgIdxs;
      SmallVector<uint32_t, 8> supportOnlyMissingArgIdxs;
      SmallVector<uint32_t, 8> mismatchedExpectedArgIdxs;
      SmallVector<uint32_t, 8> unexpectedReplayArgIdxs;

      // Build a detailed mismatch ledger before the hard equality checks.
      // These traces make it clear whether failure came from missing
      // support-only formals, actual rewrite mismatches, or unexpected
      // replay-derived formals.
      for (const auto &kvLocal : *expectedRootFormals) {
        auto it = replayRootFormals->find(kvLocal.first);
        if (it == replayRootFormals->end()) {
          missingExpectedArgIdxs.push_back(kvLocal.first);
          if (kvLocal.second.oldText == kvLocal.second.newText)
            supportOnlyMissingArgIdxs.push_back(kvLocal.first);

          if (newRangesOpt &&
              kvLocal.first < ctx.rootInvocationArgRanges.size() &&
              kvLocal.first < newRangesOpt->size()) {
            const auto &oldR = ctx.rootInvocationArgRanges[kvLocal.first];
            const auto &newR = (*newRangesOpt)[kvLocal.first];
            if (oldR.first <= oldR.second && oldR.second <= baseText.size() &&
                newR.first <= newR.second && newR.second <= newText.size()) {
              StringRef concreteOld =
                  baseText.slice((size_t)oldR.first, (size_t)oldR.second)
                      .trim();
              StringRef concreteNew =
                  newText.slice((size_t)newR.first, (size_t)newR.second).trim();
              if (concreteOld == concreteNew &&
                  concreteOld == StringRef(kvLocal.second.oldText).trim() &&
                  concreteNew == StringRef(kvLocal.second.newText).trim()) {
                unchangedConcreteMissingArgIdxs.push_back(kvLocal.first);
              }
              trace("macro/proof",
                    "{0}: root proof missing expected arg root id={1} "
                    "name={2} argIdx={3} concreteOld='{4}' "
                    "concreteNew='{5}' expectedOld='{6}' "
                    "expectedNew='{7}'",
                    traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                    kvLocal.first,
                    stringutils::showWsWithClip(concreteOld, 120),
                    stringutils::showWsWithClip(concreteNew, 120),
                    stringutils::showWsWithClip(kvLocal.second.oldText, 120),
                    stringutils::showWsWithClip(kvLocal.second.newText, 120));
            }
          }
          continue;
        }

        if (it->second.oldText != kvLocal.second.oldText ||
            it->second.newText != kvLocal.second.newText) {
          mismatchedExpectedArgIdxs.push_back(kvLocal.first);
          trace("macro/proof",
                "{0}: root proof mismatched expected arg root id={1} "
                "name={2} argIdx={3} derivedOld='{4}' derivedNew='{5}' "
                "expectedOld='{6}' expectedNew='{7}'",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                kvLocal.first,
                stringutils::showWsWithClip(it->second.oldText, 120),
                stringutils::showWsWithClip(it->second.newText, 120),
                stringutils::showWsWithClip(kvLocal.second.oldText, 120),
                stringutils::showWsWithClip(kvLocal.second.newText, 120));
        }
      }

      for (const auto &kvLocal : *replayRootFormals) {
        if (!expectedRootFormals->contains(kvLocal.first))
          unexpectedReplayArgIdxs.push_back(kvLocal.first);
      }

      llvm::sort(missingExpectedArgIdxs);
      llvm::sort(unchangedConcreteMissingArgIdxs);
      llvm::sort(supportOnlyMissingArgIdxs);
      llvm::sort(mismatchedExpectedArgIdxs);
      llvm::sort(unexpectedReplayArgIdxs);
      trace("macro/proof",
            "{0}: root proof mismatch analysis root id={1} name={2} "
            "missingExpectedArgs={3} unchangedConcreteMissingArgs={4} "
            "supportOnlyMissingArgs={5} mismatchedExpectedArgs={6} "
            "unexpectedReplayArgs={7}",
            traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
            formatUInt32List(missingExpectedArgIdxs),
            formatUInt32List(unchangedConcreteMissingArgIdxs),
            formatUInt32List(supportOnlyMissingArgIdxs),
            formatUInt32List(mismatchedExpectedArgIdxs),
            formatUInt32List(unexpectedReplayArgIdxs));
    }

    // From this point on, replay and expected metadata must be exactly
    // the same root-formal proof set. The diagnostics above explain any
    // mismatch; these checks enforce the invariant.
    if (replayRootFormals->size() != expectedRootFormals->size()) {
      cert.detail =
          formatv("{0}: root proof validation failed root id={1} "
                  "name='{2}' replay-derived root formal count "
                  "mismatch derived={3} expected={4}",
                  traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                  replayRootFormals->size(), expectedRootFormals->size())
              .str();
      return cert;
    }

    for (const auto &kvLocal : *expectedRootFormals) {
      auto it = replayRootFormals->find(kvLocal.first);
      if (it == replayRootFormals->end() ||
          it->second.oldText != kvLocal.second.oldText ||
          it->second.newText != kvLocal.second.newText) {
        cert.detail = formatv("{0}: root proof validation failed root "
                              "id={1} name='{2}' replay-derived root "
                              "formal mismatch argIdx={3} derivedOld='{4}' "
                              "derivedNew='{5}' expectedOld='{6}' "
                              "expectedNew='{7}'",
                              traceStage, ctx.rootInvocation.id,
                              ctx.rootInvocation.name, kvLocal.first,
                              it == replayRootFormals->end()
                                  ? StringRef("")
                                  : StringRef(it->second.oldText),
                              it == replayRootFormals->end()
                                  ? StringRef("")
                                  : StringRef(it->second.newText),
                              kvLocal.second.oldText, kvLocal.second.newText)
                          .str();
        return cert;
      }
    }
  }

  // Re-run the normal root invocation certificate on the replay-derived
  // formals. This ensures the concrete replacement is accepted by the
  // same root proof rules as an ordinary structured root rewrite.
  cert.replayInvocationCertificate =
      deps_.structuredLifter.BuildInvocationRewriteCertificate(
          ctx, ctx.m, *replayRootFormals, traceStage, baseText,
          ctx.invArgRanges, deferOccurrenceArgIdxs);
  if (cert.replayInvocationCertificate.kind ==
      InvocationRewriteCertificateKind::Invalid) {
    if (cert.replayInvocationCertificate.failure ==
        InvocationRewriteFailure::PasteMismatch) {
      // If the plain replay fails only on paste shape, try the narrower
      // wrapper-placeholder replay. Accept it only when it reconstructs
      // exactly the concrete replacement text being validated.
      auto wrapperReplayCert =
          deps_.structuredLifter
              .BuildWrapperPlaceholderHopInvocationCertificate(
                  ctx, ctx.m, *replayRootFormals, traceStage, baseText,
                  ctx.invArgRanges, deferOccurrenceArgIdxs);
      if (wrapperReplayCert.kind == InvocationRewriteCertificateKind::Unique &&
          !wrapperReplayCert.rewrittenInvocationSyntax.empty() &&
          StringRef(wrapperReplayCert.rewrittenInvocationSyntax).trim() ==
              newText.trim()) {
        std::string deferredPasteDetail;
        if (this->RootDeferredPasteReplayHasOnlyProvenDependentUses(
                ctx, *replayRootFormals, traceStage, deferredPasteDetail)) {
          REFOLD_LOG_TRACE("macro/proof",
                           "{0}: root proof validation accepted wrapper replay "
                           "candidate root id={1} name={2} syntax='{3}' "
                           "pasteDeferred={4}",
                           traceStage, ctx.rootInvocation.id,
                           ctx.rootInvocation.name,
                           wrapperReplayCert.rewrittenInvocationSyntax,
                           wrapperReplayCert.pasteValidation.deferred ? 1 : 0);
          cert.replayInvocationCertificate = std::move(wrapperReplayCert);
        } else {
          REFOLD_LOG_TRACE("macro/proof", "{0}", deferredPasteDetail);
          cert.detail = deferredPasteDetail;
        }
      }
    }
  }
  if (cert.replayInvocationCertificate.kind ==
      InvocationRewriteCertificateKind::Invalid) {
    if (cert.detail.empty())
      cert.detail = cert.replayInvocationCertificate.detail;
    return cert;
  }

  cert.replayRootFormals = std::move(*replayRootFormals);
  cert.valid = true;
  cert.detail =
      formatv("{0}: root proof validation succeeded root id={1} "
              "name='{2}' replayFormals={3} deferredArgs={4}",
              traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
              cert.replayRootFormals.size(), deferOccurrenceArgIdxs.size())
          .str();
  return cert;
}

bool RefoldMacroDAGCandidateValidator::ValidateDagCandidateProof(
    const RefoldMacroDAGLiftingContext &ctx,
    const DagCandidateAcceptanceContext &accCtx,
    const DagCandidateValidationMetadata &validation, StringRef baseText,
    StringRef newText, StringRef traceStage) const {
  const RefoldModel::MacroInvocation &validationRootInvocation =
      accCtx.subtreeValidation.rootInvocation;
  if (inTraceMode()) {
    // Emit a stable proof ledger before any rejection so failed composed
    // candidates can be diagnosed against the expected root-formal set
    // and deferred occurrence arguments.
    SmallVector<uint32_t, 8> expectedRootArgIdxs;
    expectedRootArgIdxs.reserve(validation.expectedRootFormals.size());
    for (const auto &kvLocal : validation.expectedRootFormals)
      expectedRootArgIdxs.push_back(kvLocal.first);
    llvm::sort(expectedRootArgIdxs);
    SmallVector<uint32_t, 8> deferredArgs = validation.deferOccurrenceArgIdxs;
    llvm::sort(deferredArgs);
    trace("macro/proof",
          "{0}: DAG candidate proof ledger enter root id={1} name={2} "
          "expectedRootArgs={3} deferredArgs={4} bridgeSensitive={5} "
          "mixed={6}",
          traceStage, validationRootInvocation.id,
          validationRootInvocation.name, formatUInt32List(expectedRootArgIdxs),
          formatUInt32List(deferredArgs),
          validation.hasBridgeSensitiveStructuredSemantics ? 1 : 0,
          validation.hasMixedSemanticInteractions ? 1 : 0);
  }

  // These semantic hazards are not repaired by root replay. If they
  // survived candidate metadata merging, the composed candidate is
  // inadmissible before any textual validation is attempted.
  if (validation.hasMixedSemanticInteractions) {
    return false;
  }
  if (validation.hasBridgeSensitiveStructuredSemantics) {
    return false;
  }

  // Re-derive the root-formal rewrite map from the concrete candidate
  // text and require it to match the expected merged proof metadata.
  auto proofCert = this->BuildRootProofValidationCertificate(
      ctx, baseText, newText, validation.deferOccurrenceArgIdxs,
      validation.hasExpectedRootFormals ? &validation.expectedRootFormals
                                        : nullptr,
      traceStage);
  if (!proofCert.valid)
    return false;
  return true;
}

DagCandidateValidationMetadata RefoldMacroDAGCandidateValidator::
    BuildDagCandidateValidationMetadataFromSubtree(
        const RefoldMacroDAGLiftingContext &ctx,
        const SubtreeRewriteCertificate &subtreeCert) const {
  DagCandidateValidationMetadata validation;
  validation.hasExpectedRootFormals = true;
  validation.hasBridgeSensitiveStructuredSemantics =
      subtreeCert.semantic.hasBridgeSensitiveStructuredSemantics;
  validation.hasMixedSemanticInteractions =
      subtreeCert.semantic.interactionSummary.hasMixedInteractions;

  // Only bridge-derived formals need their semantic signatures carried
  // into candidate-level metadata. Non-bridged formals have already been
  // checked inside the subtree semantic certificate.
  for (const auto &formalConsistency :
       subtreeCert.semantic.formalInteractionConsistencies) {
    std::string formalKey =
        formatv("{0}#{1}",
                formalConsistency.inv ? formalConsistency.inv->id : 0,
                formalConsistency.argIdx)
            .str();
    if (subtreeCert.semantic.bridgedFormalKeys.contains(formalKey))
      validation.bridgeSensitiveFormalSignatures[formalKey] =
          formalConsistency.signature;
  }

  // These are the root rewrites the concrete DAG candidate must replay
  // exactly during final proof validation.
  for (const auto &kvLocal : subtreeCert.rootFormals)
    validation.expectedRootFormals[kvLocal.first] = kvLocal.second;

  validation.deferOccurrenceArgIdxs.assign(
      subtreeCert.deferRootOccurrenceArgIdxs.begin(),
      subtreeCert.deferRootOccurrenceArgIdxs.end());
  return validation;
}

DagCandidateAcceptanceCertificate
RefoldMacroDAGCandidateValidator::AcceptOrMergeDAGCandidatePatch(
    const RefoldMacroDAGLiftingContext &ctx,
    DagCandidateAcceptanceContext &accCtx, MacroPatch candPatch,
    StringRef baseText, StringRef traceStage,
    const DagCandidateValidationMetadata *candValidation) const {
  DagCandidateAcceptanceCertificate cert;
  DagCandidateValidationMetadata candidateValidation;
  if (candValidation)
    candidateValidation = *candValidation;

  // First candidate for this root span: validate it directly, certify the
  // root proof identity onto the patch, and install it as the unique
  // candidate state.
  if (!accCtx.uniquePatch) {
    if (!this->ValidateDagCandidateProof(ctx, accCtx, candidateValidation,
                                         baseText, candPatch.replacement,
                                         traceStage)) {
      cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
      cert.detail =
          formatv("{0}: DAG candidate patch rejected root id={1} "
                  "name={2} semantic validation metadata failed",
                  traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
              .str();
      return cert;
    }
    if (!candPatch.macroId)
      candPatch.macroId = ctx.m.id;
    if (!candPatch.proof.proofRootMacroId) {
      candPatch.proof.proofRootMacroId = ctx.m.id;
      deps_.proofLattice.MacroPatchProofClassifier().SyncMacroPatchProofSummary(
          candPatch);
    }
    accCtx.uniquePatch = std::move(candPatch);
    accCtx.uniquePatchBaseText = baseText.str();
    accCtx.uniquePatchValidation = std::move(candidateValidation);
    accCtx.distinctRootPatches = 1;
    cert.accepted = true;
    cert.detail =
        formatv("{0}: accepted first DAG candidate root patch "
                "root id={1} name={2} span=[{3},{4}) replLen={5}",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                accCtx.uniquePatch->invRange.begin,
                accCtx.uniquePatch->invRange.end,
                accCtx.uniquePatch->replacement.size())
            .str();
    return cert;
  }

  // All DAG candidates for one accepted root patch must cover the exact
  // same source invocation span. Different spans are not composable here.
  if (accCtx.uniquePatch->invRange.begin != candPatch.invRange.begin ||
      accCtx.uniquePatch->invRange.end != candPatch.invRange.end) {
    cert.failure = DagCandidateAcceptanceFailure::DifferentSpan;
    cert.detail =
        formatv("{0}: DAG candidate patch rejected root id={1} "
                "name={2} span mismatch existing=[{3},{4}) "
                "candidate=[{5},{6})",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                accCtx.uniquePatch->invRange.begin,
                accCtx.uniquePatch->invRange.end, candPatch.invRange.begin,
                candPatch.invRange.end)
            .str();
    return cert;
  }

  // Textually equivalent patches are still proof-relevant. Merge their
  // validation metadata and replay-validate the unchanged replacement so
  // equivalent subtrees cannot smuggle incompatible semantic obligations.
  if (accCtx.uniquePatch->replacement == candPatch.replacement) {
    auto mergedValidation = this->MergeDagCandidateValidationMetadata(
        ctx, accCtx.uniquePatchValidation, candidateValidation);
    if (accCtx.uniquePatch->subtree.backed || candPatch.subtree.backed) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "DAG equivalent subtree-plan probe: root id={0} name={1} "
          "stage={2} existingExpRoot={3} candidateExpRoot={4} "
          "mergedExpRoot(pending) currentDeferredArgs={5} "
          "candidateDeferredArgs={6}",
          ctx.rootInvocation.id, ctx.rootInvocation.name, traceStage,
          stringutils::showWsWithClip(
              accCtx.uniquePatch->subtree.expectedRootFormalSummary, 160),
          stringutils::showWsWithClip(
              candPatch.subtree.expectedRootFormalSummary, 160),
          stringutils::showWsWithClip(
              accCtx.uniquePatch->subtree.deferredRootArgSummary, 160),
          stringutils::showWsWithClip(candPatch.subtree.deferredRootArgSummary,
                                      160));
    }
    if (!mergedValidation) {
      cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
      cert.detail =
          formatv("{0}: DAG candidate patch rejected root id={1} "
                  "name={2} equivalent replacement produced "
                  "incompatible root-formal validation metadata",
                  traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
              .str();
      return cert;
    }

    if (!this->ValidateDagCandidateProof(
            ctx, accCtx, *mergedValidation, *accCtx.uniquePatchBaseText,
            accCtx.uniquePatch->replacement, traceStage)) {
      cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
      cert.detail =
          formatv("{0}: DAG candidate patch rejected root id={1} "
                  "name={2} equivalent replacement failed merged "
                  "root validation",
                  traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
              .str();
      return cert;
    }

    if (accCtx.uniquePatch->subtree.backed || candPatch.subtree.backed) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "DAG equivalent subtree-plan merged: root id={0} name={1} "
          "stage={2} mergedExpRoot={3} mergedDeferredArgs={4} "
          "mergedBridgeFormals={5}",
          ctx.rootInvocation.id, ctx.rootInvocation.name, traceStage,
          deps_.invertibilitySolver.FormatFormalTextPairMap(
              mergedValidation->expectedRootFormals),
          formatUInt32List(mergedValidation->deferOccurrenceArgIdxs),
          this->FormatBridgeSensitiveFormalSignatureMap(
              mergedValidation->bridgeSensitiveFormalSignatures));
    }
    unionMacroPatchMaterializedBTokenRange(*accCtx.uniquePatch, candPatch);
    accCtx.uniquePatchValidation = std::move(*mergedValidation);
    cert.accepted = true;
    cert.detail =
        formatv("{0}: DAG candidate patch equivalent to existing "
                "root patch root id={1} name={2} span=[{3},{4})",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                accCtx.uniquePatch->invRange.begin,
                accCtx.uniquePatch->invRange.end)
            .str();
    return cert;
  }

  // Non-equivalent replacements can be merged only if they were produced
  // from the same original root invocation spelling.
  if (!accCtx.uniquePatchBaseText || *accCtx.uniquePatchBaseText != baseText) {
    cert.failure = DagCandidateAcceptanceFailure::DifferentBaseText;
    cert.detail =
        formatv("{0}: DAG candidate patch rejected root id={1} "
                "name={2} base text mismatch baseLenExisting={3} "
                "baseLenCandidate={4}",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name,
                accCtx.uniquePatchBaseText ? accCtx.uniquePatchBaseText->size()
                                           : 0,
                baseText.size())
            .str();
    return cert;
  }

  // Before attempting textual merge, check whether the candidates are the
  // same proof shape but one preserves strictly more invocation
  // structure. This handles wrapper-preserving vs. flattened alternatives
  // without treating them as arbitrary conflicting text hunks.
  int preferredStructured = this->ChoosePreferredStructuredDagCandidate(
      ctx, accCtx.uniquePatchValidation, candidateValidation);
  if (preferredStructured < 0) {
    if (accCtx.uniquePatch->subtree.backed || candPatch.subtree.backed) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "DAG structured subtree-choice kept existing: root id={0} "
          "name={1} stage={2} existingExpRoot={3} candidateExpRoot={4}",
          ctx.rootInvocation.id, ctx.rootInvocation.name, traceStage,
          stringutils::showWsWithClip(
              accCtx.uniquePatch->subtree.expectedRootFormalSummary, 160),
          stringutils::showWsWithClip(
              candPatch.subtree.expectedRootFormalSummary, 160));
    }
    cert.accepted = true;
    cert.detail =
        formatv("{0}: kept existing structured DAG candidate root "
                "patch root id={1} name={2} over flatter rival",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
            .str();
    return cert;
  }
  if (preferredStructured > 0) {
    if (!candPatch.macroId)
      candPatch.macroId = ctx.m.id;
    if (!candPatch.proof.proofRootMacroId) {
      candPatch.proof.proofRootMacroId = ctx.m.id;
      deps_.proofLattice.MacroPatchProofClassifier().SyncMacroPatchProofSummary(
          candPatch);
    }
    if (accCtx.uniquePatch->subtree.backed || candPatch.subtree.backed) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "DAG structured subtree-choice replaced existing: root id={0} "
          "name={1} stage={2} existingExpRoot={3} candidateExpRoot={4}",
          ctx.rootInvocation.id, ctx.rootInvocation.name, traceStage,
          stringutils::showWsWithClip(
              accCtx.uniquePatch->subtree.expectedRootFormalSummary, 160),
          stringutils::showWsWithClip(
              candPatch.subtree.expectedRootFormalSummary, 160));
    }
    accCtx.uniquePatch = std::move(candPatch);
    accCtx.uniquePatchBaseText = baseText.str();
    accCtx.uniquePatchValidation = std::move(candidateValidation);
    cert.accepted = true;
    cert.detail =
        formatv("{0}: replaced existing DAG candidate root patch "
                "root id={1} name={2} with more structured rival",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
            .str();
    return cert;
  }

  // Neither candidate dominates structurally, so try ordinary compatible
  // replacement merging. The text merge and the proof-metadata merge must
  // both succeed, and the merged replacement must replay through the root
  // proof validator.
  SmallVector<StringRef, 2> repls;
  repls.push_back(StringRef(accCtx.uniquePatch->replacement));
  repls.push_back(StringRef(candPatch.replacement));
  auto merged = mergeCompatibleStringReplacements(*accCtx.uniquePatchBaseText,
                                                  ArrayRef<StringRef>(repls));
  if (!merged) {
    cert.failure = DagCandidateAcceptanceFailure::MergeConflict;
    cert.detail =
        formatv("{0}: DAG candidate patch rejected root id={1} "
                "name={2} incompatible replacement hunks",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
            .str();
    return cert;
  }

  auto mergedValidation = this->MergeDagCandidateValidationMetadata(
      ctx, accCtx.uniquePatchValidation, candidateValidation);
  if (accCtx.uniquePatch->subtree.backed || candPatch.subtree.backed) {
    REFOLD_LOG_TRACE(
        "macro/proof",
        "DAG merge subtree-plan probe: root id={0} name={1} stage={2} "
        "existingExpRoot={3} candidateExpRoot={4}",
        ctx.rootInvocation.id, ctx.rootInvocation.name, traceStage,
        stringutils::showWsWithClip(
            accCtx.uniquePatch->subtree.expectedRootFormalSummary, 160),
        stringutils::showWsWithClip(candPatch.subtree.expectedRootFormalSummary,
                                    160));
  }
  if (!mergedValidation) {
    cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
    cert.detail =
        formatv("{0}: DAG candidate patch rejected root id={1} "
                "name={2} merged replacement produced "
                "incompatible root-formal validation metadata",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
            .str();
    return cert;
  }

  if (accCtx.uniquePatch->subtree.backed || candPatch.subtree.backed) {
    REFOLD_LOG_TRACE(
        "macro/proof",
        "DAG merge subtree-plan merged: root id={0} name={1} stage={2} "
        "mergedExpRoot={3} mergedDeferredArgs={4} "
        "mergedBridgeFormals={5}",
        ctx.rootInvocation.id, ctx.rootInvocation.name, traceStage,
        deps_.invertibilitySolver.FormatFormalTextPairMap(
            mergedValidation->expectedRootFormals),
        formatUInt32List(mergedValidation->deferOccurrenceArgIdxs),
        this->FormatBridgeSensitiveFormalSignatureMap(
            mergedValidation->bridgeSensitiveFormalSignatures));
  }
  if (!this->ValidateDagCandidateProof(ctx, accCtx, *mergedValidation,
                                       *accCtx.uniquePatchBaseText,
                                       StringRef(*merged), traceStage)) {
    cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
    cert.detail =
        formatv("{0}: DAG candidate patch rejected root id={1} "
                "name={2} merged replacement failed root "
                "validation",
                traceStage, ctx.rootInvocation.id, ctx.rootInvocation.name)
            .str();
    return cert;
  }

  // The merged candidate is now both textually composable and
  // proof-valid. Update the unique patch in place while preserving the
  // accumulated validation metadata for any later candidate.
  accCtx.uniquePatch->replacement = std::move(*merged);
  accCtx.uniquePatch->materialized.hasOutputByteRange = false;
  unionMacroPatchMaterializedBTokenRange(*accCtx.uniquePatch, candPatch);
  accCtx.uniquePatchValidation = std::move(*mergedValidation);
  if (!accCtx.uniquePatch->macroId)
    accCtx.uniquePatch->macroId = candPatch.macroId;
  ++accCtx.distinctRootPatches;
  cert.accepted = true;
  cert.merged = true;
  cert.detail = formatv("{0}: merged DAG candidate root patch root id={1} "
                        "name={2} accCtx.distinctRootPatches={3} replLen={4}",
                        traceStage, ctx.rootInvocation.id,
                        ctx.rootInvocation.name, accCtx.distinctRootPatches,
                        accCtx.uniquePatch->replacement.size())
                    .str();
  return cert;
}

} // namespace refold
} // namespace clang
