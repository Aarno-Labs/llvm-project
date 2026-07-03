//===--- RefoldMacroDAGInvertibilitySolver.cpp ----------------*- C++ -*-===//
//
// Formal/argument invertibility solver for macro DAG lifting.
//
// This translation unit owns the proof checks that decide whether an observed
// rewrite in a nested callee can be inverted through argument references, paste
// spans, stringification surfaces, and raw formal text back to caller formals.
// It consumes text primitives through Dependencies and receives the exact token
// hunk slice for each proof request explicitly, so the certificate produced by
// each method is tied to the caller-provided DAG lifting context.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDAGInvertibilitySolver.h"

#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

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

RefoldMacroDAGInvertibilitySolver::RefoldMacroDAGInvertibilitySolver(
    Dependencies deps)
    : deps_(std::move(deps)) {}

RefoldMacroPasteArgumentBuilder
RefoldMacroDAGInvertibilitySolver::pasteArgumentBuilder() const {
  return RefoldMacroPasteArgumentBuilder(
      {&deps_.sourceMapper, deps_.aToks, deps_.bToks, &deps_.lexLang});
}

bool RefoldMacroDAGInvertibilitySolver::SameIndexSet(
    ArrayRef<uint32_t> a, ArrayRef<uint32_t> b) const {
  SmallVector<uint32_t, 4> sa(a.begin(), a.end());
  SmallVector<uint32_t, 4> sb(b.begin(), b.end());
  llvm::sort(sa);
  llvm::sort(sb);
  sa.erase(std::unique(sa.begin(), sa.end()), sa.end());
  sb.erase(std::unique(sb.begin(), sb.end()), sb.end());
  return sa == sb;
}

ArgRefInvertibilityCertificate
RefoldMacroDAGInvertibilitySolver::BuildArgRefInvertibilityCertificate(
    const ArgRefTemplate &tpl, StringRef observed) const {
  ArgRefInvertibilityCertificate cert;

  constexpr size_t maxDistinctCallerParams = 8;

  // Empty templates do not prove forwarding, and very wide templates are
  // kept out of this local DFS to avoid turning malformed metadata into
  // an expensive search problem.
  if (tpl.refs.empty() || tpl.distinctCallerParams.empty() ||
      tpl.distinctCallerParams.size() > maxDistinctCallerParams)
    return cert;

  // Decompose the template into:
  //
  //   literal[0], var[0], literal[1], var[1], ..., literal[n]
  //
  // `varOrdinals` indexes into `tpl.distinctCallerParams`, so repeated
  // refs to the same caller parameter share one assignment slot.
  SmallVector<StringRef, 8> literals;
  SmallVector<unsigned, 8> varOrdinals;
  literals.reserve(tpl.refs.size() + 1);
  varOrdinals.reserve(tpl.refs.size());

  size_t curPos = 0;
  for (const auto &ref : tpl.refs) {
    if (ref.begin < curPos || ref.end < ref.begin ||
        ref.end > tpl.argText.size()) {
      cert.kind = ArgRefInvertibilityKind::NoMatch;
      return cert;
    }

    literals.push_back(StringRef(tpl.argText).slice(curPos, ref.begin));

    auto it = llvm::find(tpl.distinctCallerParams, ref.callerParamIndex);
    if (it == tpl.distinctCallerParams.end()) {
      cert.kind = ArgRefInvertibilityKind::NoMatch;
      return cert;
    }

    varOrdinals.push_back(
        (unsigned)std::distance(tpl.distinctCallerParams.begin(), it));
    curPos = ref.end;
  }
  literals.push_back(StringRef(tpl.argText).drop_front(curPos));

  StringRef obs = observed.trim();

  // `assigns[i]` is the candidate observed text for
  // `tpl.distinctCallerParams[i]`. It remains empty until the DFS first
  // reaches that caller parameter placeholder.
  SmallVector<std::optional<StringRef>, maxDistinctCallerParams> assigns(
      tpl.distinctCallerParams.size());

  // Keep at most enough distinct solutions to distinguish Unique from
  // Ambiguous. Duplicate assignment vectors can arise through equivalent
  // split paths and are ignored.
  SmallVector<SmallVector<std::string, maxDistinctCallerParams>, 2> solutions;

  auto addSolution = [&](ArrayRef<std::optional<StringRef>> aLocal) {
    SmallVector<std::string, maxDistinctCallerParams> sLocal;
    sLocal.reserve(tpl.distinctCallerParams.size());
    for (size_t i = 0; i < tpl.distinctCallerParams.size(); ++i)
      sLocal.push_back(aLocal[i] ? aLocal[i]->str() : std::string());

    for (const auto &existing : solutions)
      if (existing == sLocal)
        return;

    solutions.push_back(std::move(sLocal));
  };

  auto dfs = [&](auto &&self, size_t refIdx, size_t obsPos) -> void {
    // One solution is acceptable; a second distinct solution is enough to
    // prove ambiguity, so stop exploring once ambiguity is known.
    if (solutions.size() > 1)
      return;

    // Each placeholder is preceded by a fixed literal. The observed text
    // must match that literal exactly at the current position before the
    // variable can consume anything.
    const StringRef lit = literals[refIdx];
    if (obsPos > obs.size() || !obs.drop_front(obsPos).starts_with(lit))
      return;
    obsPos += lit.size();

    // All placeholders consumed. This path is a solution only if it also
    // consumed the full observed text, including the trailing literal.
    if (refIdx == varOrdinals.size()) {
      if (obsPos == obs.size())
        addSolution(assigns);
      return;
    }

    const unsigned varOrd = varOrdinals[refIdx];
    if (varOrd >= assigns.size())
      return;

    if (assigns[varOrd]) {
      // Repeated references to the same caller parameter must consume the
      // exact same observed text as the first occurrence.
      const StringRef val = *assigns[varOrd];
      if (obsPos <= obs.size() && obs.drop_front(obsPos).starts_with(val))
        self(self, refIdx + 1, obsPos + val.size());
      return;
    }

    // Bound the candidate length by the fixed literals and already-bound
    // variables that must still fit in the remaining observed text.
    // Unbound later variables may be empty, so they do not add to this
    // lower bound.
    size_t minRemain = 0;
    for (size_t j = refIdx + 1; j < literals.size(); ++j)
      minRemain += literals[j].size();
    for (size_t j = refIdx + 1; j < varOrdinals.size(); ++j) {
      const unsigned laterOrd = varOrdinals[j];
      if (laterOrd < assigns.size() && assigns[laterOrd])
        minRemain += assigns[laterOrd]->size();
    }
    if (obsPos + minRemain > obs.size())
      return;

    const size_t maxLen = obs.size() - obsPos - minRemain;
    const StringRef rest = obs.drop_front(obsPos);
    const StringRef nextLit = literals[refIdx + 1];

    auto tryLen = [&](size_t len) {
      assigns[varOrd] = rest.take_front(len);
      self(self, refIdx + 1, obsPos + len);
      assigns[varOrd] = std::nullopt;
    };

    if (!nextLit.empty()) {
      // When the next literal is known, only split at occurrences of that
      // literal. This avoids enumerating equivalent impossible lengths.
      for (size_t searchPos = 0;; ++searchPos) {
        const size_t pos = rest.find(nextLit, searchPos);
        if (pos == StringRef::npos || pos > maxLen)
          break;
        tryLen(pos);
        if (solutions.size() > 1)
          return;
      }
    } else {
      // With no following literal delimiter, every remaining length is a
      // possible assignment; uniqueness below decides whether this is
      // safe.
      for (size_t len = 0; len <= maxLen; ++len) {
        tryLen(len);
        if (solutions.size() > 1)
          return;
      }
    }
  };

  dfs(dfs, 0, 0);

  // Exactly one assignment vector is required. Zero solutions means this
  // observed text does not realize the template; multiple means
  // ambiguous.
  if (solutions.empty()) {
    cert.kind = ArgRefInvertibilityKind::NoMatch;
    return cert;
  }
  if (solutions.size() > 1) {
    cert.kind = ArgRefInvertibilityKind::Ambiguous;
    return cert;
  }

  cert.kind = ArgRefInvertibilityKind::Unique;
  for (unsigned i = 0; i < tpl.distinctCallerParams.size(); ++i)
    cert.derivedTextByCallerParam[tpl.distinctCallerParams[i]] =
        solutions[0][i];
  return cert;
}

std::optional<ArgInvertibilityCertificate>
RefoldMacroDAGInvertibilitySolver::BuildArgInvertibilityCertificate(
    const RefoldModel::MacroInvocation &parent, uint32_t parentFormal,
    StringRef observedOld0) const {
  auto argInfo =
      deps_.textPrimitives.GetTrimmedInvocationArgInfo(parent, parentFormal);
  if (!argInfo)
    return std::nullopt;

  StringRef rawArg = StringRef(argInfo->text).trim();
  StringRef observedOld = observedOld0.trim();

  ArgInvertibilityCertificate cert;
  cert.rawArgText = rawArg.str();

  // Discover child invocations spelled directly inside this parent
  // argument. These become template slots; the text between them remains
  // fixed literal material.
  auto placeholders = deps_.textPrimitives.GetTopLevelLexicalChildrenInArg(
      parent, parentFormal);

  if (placeholders.empty()) {
    // No child slots means the argument is just literal surface text. It
    // is invertible only when the observed old expansion equals that text
    // exactly.
    if (observedOld != rawArg)
      return std::nullopt;
    cert.kind = ArgInvertibilityKind::LiteralOnly;
    cert.literals.push_back(rawArg.str());
    return cert;
  }

  cert.kind = ArgInvertibilityKind::TemplateWithChildren;
  cert.slots = placeholders;

  // Decompose the raw argument into alternating fixed literals and child
  // slots:
  //
  //   literal[0], slot[0], literal[1], slot[1], ..., literal[n]
  //
  // Slot ranges are relative to the trimmed parent argument.
  uint64_t curPos = 0;
  for (const auto &ph : placeholders) {
    if (ph.relBegin < curPos || ph.relEnd < ph.relBegin ||
        ph.relEnd > rawArg.size())
      return std::nullopt;

    cert.literals.push_back(
        rawArg.slice((size_t)curPos, (size_t)ph.relBegin).str());
    curPos = ph.relEnd;
  }
  cert.literals.push_back(rawArg.drop_front((size_t)curPos).str());

  SmallVector<unsigned, 4> chosenOld;
  SmallVector<SmallVector<unsigned, 4>, 2> oldSolutions;

  // Match the observed old expansion against the literal/slot template
  // and record which observed wrapper form each child slot used.
  auto matchOld = [&](auto &&self, size_t idx, size_t pos) -> void {
    // Stop after finding more than one solution; the later equivalence
    // check only needs to distinguish unique/semantically-equivalent from
    // ambiguous.
    if (oldSolutions.size() > 1)
      return;

    // Each slot is preceded by a fixed literal fragment that must match
    // exactly at the current observed position.
    const StringRef lit = cert.literals[idx];
    if (pos > observedOld.size() ||
        !observedOld.drop_front(pos).starts_with(lit))
      return;
    pos += lit.size();

    if (idx == cert.slots.size()) {
      // All slots consumed. This is a complete match only if the
      // trailing literal also consumed the rest of the observed old text.
      if (pos == observedOld.size())
        oldSolutions.push_back(chosenOld);
      return;
    }

    // Try every certified old surface for this child slot. A slot may
    // match as the child expansion, raw child invocation, string literal
    // wrapper, etc.; the selected form is recorded so the rewrite can
    // preserve the same wrapper shape later.
    for (unsigned choice = 0; choice < cert.slots[idx].observedForms.size();
         ++choice) {
      StringRef phOld = cert.slots[idx].observedForms[choice].observedOldText;
      if (observedOld.drop_front(pos).starts_with(phOld)) {
        chosenOld.push_back(choice);
        self(self, idx + 1, pos + phOld.size());
        chosenOld.pop_back();
      }
    }
  };

  matchOld(matchOld, 0, 0);
  if (oldSolutions.empty())
    return std::nullopt;

  auto semanticallyEquivalentOldSolutions =
      [&](const SmallVectorImpl<unsigned> &a,
          const SmallVectorImpl<unsigned> &b) -> bool {
    if (a.size() != b.size())
      return false;

    for (size_t i = 0; i < a.size(); ++i) {
      if (i >= cert.slots.size() ||
          a[i] >= cert.slots[i].observedForms.size() ||
          b[i] >= cert.slots[i].observedForms.size()) {
        return false;
      }

      const auto &fa = cert.slots[i].observedForms[a[i]];
      const auto &fb = cert.slots[i].observedForms[b[i]];

      // Multiple textual matches are acceptable only when they select the
      // same wrapper semantics and the same logical child input.
      // Otherwise the old observation is ambiguous and cannot drive a
      // deterministic rewrite.
      if (fa.kind != fb.kind || fa.source != fb.source ||
          StringRef(fa.logicalInputText).trim() !=
              StringRef(fb.logicalInputText).trim()) {
        return false;
      }
    }

    return true;
  };

  // Accept multiple syntactic matches only when they are semantically
  // identical for every slot. This avoids rejecting harmless duplicate
  // surfaces while still failing closed on genuinely different wrapper
  // interpretations.
  for (size_t i = 1; i < oldSolutions.size(); ++i) {
    if (!semanticallyEquivalentOldSolutions(oldSolutions[0], oldSolutions[i])) {
      return std::nullopt;
    }
  }

  cert.chosenObservedFormIdx = oldSolutions[0];

  return cert;
}

ArgSemanticRewriteCertificate
RefoldMacroDAGInvertibilitySolver::BuildArgSemanticRewriteCertificate(
    const ArgInvertibilityCertificate &cert, StringRef observedNew0,
    const DenseMap<uint64_t, std::string> *preferredChildSyntax) const {
  ArgSemanticRewriteCertificate argCert;
  StringRef observedNew = observedNew0.trim();
  argCert.observedNewText = observedNew.str();
  argCert.rawArgOldText = StringRef(cert.rawArgText).trim().str();

  // Literal-only arguments have no child slots to preserve. The new
  // observed text is therefore the new argument spelling directly.
  if (cert.kind == ArgInvertibilityKind::LiteralOnly) {
    argCert.rawArgNewText = observedNew.str();
    argCert.kind = (StringRef(argCert.rawArgNewText).trim() ==
                    StringRef(argCert.rawArgOldText).trim())
                       ? ArgSemanticRewriteCertificateKind::NoChange
                       : ArgSemanticRewriteCertificateKind::Unique;
    return argCert;
  }

  SmallVector<SlotRewriteDecision, 8> newParts;
  SmallVector<SlotSemanticRewriteCertificate, 8> newPartCertificates;
  SmallVector<SmallVector<SlotRewriteDecision, 8>, 2> newSolutions;
  SmallVector<SmallVector<SlotSemanticRewriteCertificate, 8>, 2>
      newSolutionCertificates;

  auto pieceMatchesWrapperCertificate =
      [&](const WrapperChainCertificate &wrapper, StringRef piece0) -> bool {
    // First check only the surface shape required by the wrapper selected
    // during old-text inversion: exact text, string literal, or wide
    // string literal.
    StringRef piece = piece0.trim();
    switch (wrapper.kind) {
    case WrapperChainKind::Exact:
      return true;
    case WrapperChainKind::StringLiteral:
      return stringutils::looksLikeStringLiteralToken(piece);
    case WrapperChainKind::WideStringLiteral:
      return piece.starts_with("L\"") ||
             (piece.starts_with("L") &&
              stringutils::looksLikeStringLiteralToken(piece.drop_front(1)));
    }
    llvm_unreachable("invalid WrapperChainKind");
  };

  auto literalDecodesToCanonicalLogicalInput =
      [&](StringRef piece0, StringRef expected0) -> bool {
    // Stringified children are compared through the canonical inverse so
    // equivalent escaped/whitespace-normalized payloads collapse
    // together.
    auto decoded = deps_.argTextRecovery.UnstringifyLiteralToArgText(
        piece0, /*allowTopLevelComma=*/true);
    if (!decoded)
      return false;

    auto canonDecoded =
        stringutils::canonicalizeStringifyInversePayload(*decoded);
    auto canonExpected =
        stringutils::canonicalizeStringifyInversePayload(expected0);
    if (!canonDecoded || !canonExpected)
      return false;

    // Require both sides to already be in canonical form before comparing
    // them. Otherwise a non-canonical literal spelling could be accepted
    // as if it were a unique logical child input.
    return StringRef(*canonDecoded).trim() == StringRef(*decoded).trim() &&
           StringRef(*canonExpected).trim() == expected0.trim() &&
           *canonDecoded == *canonExpected;
  };

  auto pieceMatchesWrapperLogicalInput =
      [&](const WrapperChainCertificate &wrapper, StringRef piece0) -> bool {
    // Validate both the wrapper surface and the logical child input it
    // denotes.
    if (!pieceMatchesWrapperCertificate(wrapper, piece0))
      return false;

    const StringRef logical = StringRef(wrapper.logicalInputText).trim();
    switch (wrapper.kind) {
    case WrapperChainKind::Exact:
      return piece0.trim() == logical;
    case WrapperChainKind::StringLiteral:
    case WrapperChainKind::WideStringLiteral:
      return literalDecodesToCanonicalLogicalInput(piece0, logical);
    }
    llvm_unreachable("invalid WrapperChainKind");
  };

  auto pieceMatchesAnyTrimmedCandidate =
      [&](StringRef piece0,
          const SmallVectorImpl<std::string> &candidates) -> bool {
    StringRef piece = piece0.trim();
    for (const auto &cand : candidates)
      if (piece == StringRef(cand).trim())
        return true;
    return false;
  };

  auto collectSlotSemanticRewriteCertificates =
      [&](const LexicalChildPlaceholder &slot,
          const WrapperChainCertificate &wrapper,
          StringRef piece0) -> SmallVector<SlotSemanticRewriteCertificate, 4> {
    SmallVector<SlotSemanticRewriteCertificate, 4> out;
    const StringRef piece = piece0.trim();

    auto addUnique = [&](SlotRewriteDecisionKind kind, StringRef rebuilt0) {
      SlotSemanticRewriteCertificate cert;
      cert.kind = SlotSemanticRewriteCertificateKind::Unique;
      cert.decision.kind = kind;
      cert.decision.observedText = piece.str();
      cert.decision.rebuiltText = rebuilt0.str();
      cert.wrapperKind = wrapper.kind;
      cert.wrapperSource = wrapper.source;
      cert.logicalInputText = wrapper.logicalInputText;
      for (const auto &existing : out)
        if (existing == cert)
          return;
      out.push_back(std::move(cert));
    };

    if (preferredChildSyntax && slot.child) {
      // Prefer preserving a certified child invocation spelling when it
      // is semantically compatible with the piece observed in the new
      // text.
      auto it = preferredChildSyntax->find(slot.child->id);
      if (it != preferredChildSyntax->end() &&
          pieceMatchesWrapperCertificate(wrapper, piece)) {
        bool compatible = false;
        const StringRef preferredSyntax = StringRef(it->second).trim();

        switch (wrapper.source) {
        case WrapperObservedSource::ChildRawInvocation:
          // The old slot matched the raw child invocation surface.
          // Preserving child syntax is valid only if the new piece
          // denotes that preferred invocation syntax under the same
          // wrapper form.
          switch (wrapper.kind) {
          case WrapperChainKind::Exact:
            compatible = piece == preferredSyntax;
            break;
          case WrapperChainKind::StringLiteral:
          case WrapperChainKind::WideStringLiteral:
            compatible =
                literalDecodesToCanonicalLogicalInput(piece, preferredSyntax);
            break;
          }
          break;

        case WrapperObservedSource::ChildExpansion:
          // The old slot matched the child's expansion surface. The
          // preferred child syntax is compatible only if the observed new
          // piece is still a valid new expansion candidate for that
          // child.
          switch (wrapper.kind) {
          case WrapperChainKind::Exact:
            compatible = pieceMatchesAnyTrimmedCandidate(
                piece, slot.newExpansionCandidates);
            break;
          case WrapperChainKind::StringLiteral:
          case WrapperChainKind::WideStringLiteral:
            compatible = llvm::any_of(
                slot.newExpansionCandidates, [&](const std::string &cand) {
                  return literalDecodesToCanonicalLogicalInput(piece, cand);
                });
            break;
          }
          break;
        }

        if (compatible)
          addUnique(SlotRewriteDecisionKind::PreferredChildSyntax,
                    preferredSyntax);
      }
    }

    if (!slot.rawInvocationText.empty() && !slot.observedForms.empty()) {
      // If no preferred syntax is available, retaining the original raw
      // child invocation is still valid when the new observed piece
      // realizes the same logical input under a certified wrapper form.
      for (const auto &form : slot.observedForms) {
        if (!pieceMatchesWrapperLogicalInput(form, piece))
          continue;
        addUnique(SlotRewriteDecisionKind::PreserveRawInvocation,
                  slot.rawInvocationText);
      }
    }

    return out;
  };

  auto addNewSolution =
      [&](const SmallVectorImpl<SlotRewriteDecision> &parts,
          const SmallVectorImpl<SlotSemanticRewriteCertificate>
              &slotCertificates) {
        // Deduplicate equivalent decision vectors. Different traversal
        // paths can sometimes reconstruct the same slot decisions.
        SmallVector<SlotRewriteDecision, 8> copy(parts.begin(), parts.end());
        for (const auto &existing : newSolutions)
          if (existing == copy)
            return;

        newSolutions.push_back(std::move(copy));
        newSolutionCertificates.emplace_back(slotCertificates.begin(),
                                             slotCertificates.end());
      };

  auto rebuildFromSolution =
      [&](const SmallVectorImpl<SlotRewriteDecision> &sol) -> std::string {
    // Reassemble the parent argument from fixed literals and the rebuilt
    // text selected for each child slot.
    std::string rebuilt;
    for (size_t i = 0; i < cert.slots.size(); ++i) {
      rebuilt += cert.literals[i];
      rebuilt += sol[i].rebuiltText;
    }
    rebuilt += cert.literals.back();
    return rebuilt;
  };

  auto solutionsCollapseToSameRebuilt = [&]() -> bool {
    // Multiple slot-level explanations are acceptable only if they
    // produce the exact same rebuilt parent argument text.
    if (newSolutions.empty())
      return false;
    std::string rebuilt = rebuildFromSolution(newSolutions[0]);
    for (size_t i = 1; i < newSolutions.size(); ++i)
      if (rebuildFromSolution(newSolutions[i]) != rebuilt)
        return false;
    return true;
  };

  auto solveNew = [&](auto &&self, size_t idx, size_t pos) -> void {
    // Once two distinct solutions are present, uniqueness has already
    // failed unless they later collapse to the same rebuilt argument.
    if (newSolutions.size() > 1)
      return;

    // Each slot is preceded by the fixed literal captured from the old
    // argument template. The new observed text must preserve those
    // literal boundaries.
    const StringRef lit = cert.literals[idx];
    if (pos > observedNew.size() ||
        !observedNew.drop_front(pos).starts_with(lit))
      return;
    pos += lit.size();

    if (idx == cert.slots.size()) {
      // All child slots were consumed; accept only full consumption of
      // the new observed text, including the trailing literal.
      if (pos == observedNew.size())
        addNewSolution(newParts, newPartCertificates);
      return;
    }

    const auto &slot = cert.slots[idx];

    // Reuse the old inversion's selected wrapper form for this slot. This
    // keeps the new reconstruction from silently changing a child from
    // raw invocation to expansion, or from exact text to stringized text.
    const WrapperChainCertificate wrapper =
        (idx < cert.chosenObservedFormIdx.size() &&
         cert.chosenObservedFormIdx[idx] < slot.observedForms.size())
            ? slot.observedForms[cert.chosenObservedFormIdx[idx]]
            : WrapperChainCertificate{};

    // The remaining fixed literals must fit after this slot piece. This
    // bounds the maximum slot length before we enumerate candidate
    // pieces.
    size_t minRemain = 0;
    for (size_t j = idx + 1; j < cert.literals.size(); ++j)
      minRemain += cert.literals[j].size();
    if (pos + minRemain > observedNew.size())
      return;

    const size_t maxLen = observedNew.size() - pos - minRemain;
    const StringRef rest = observedNew.drop_front(pos);
    const StringRef nextLit = cert.literals[idx + 1];

    auto enumerateSlotPieces = [&](auto &&emitPiece) {
      // Candidate slot pieces must be balanced refold fragments and must
      // end at token-like boundaries so reconstruction cannot split an
      // identifier or literal spelling accidentally.
      if (!nextLit.empty()) {
        // When the next fixed literal is known, only consider top-level
        // occurrences of that literal as the slot endpoint.
        deps_.textPrimitives.EnumerateTopLevelLiteralMatchesInRefoldText(
            rest, nextLit, maxLen, [&](size_t found) {
              StringRef piece = rest.take_front(found);
              if (!deps_.textPrimitives.IsBalancedRefoldFragment(piece))
                return;
              emitPiece(piece);
              if (newSolutions.size() > 1)
                return;
            });
      } else {
        // Without a following literal, every balanced top-level cut point
        // is a possible slot endpoint. The uniqueness checks below
        // decide whether any such split is acceptable.
        enumerateTopLevelBalancedCutPointsWithLexer(
            rest, deps_.lexLang, [&](unsigned cut) {
              const size_t len = static_cast<size_t>(cut);
              if (len > maxLen)
                return;
              if (!deps_.textPrimitives.IsLikelyTokenBoundaryInRefoldText(
                      rest, len) ||
                  !deps_.textPrimitives.IsBalancedRefoldFragment(
                      rest.take_front(len)))
                return;
              emitPiece(rest.take_front(len));
              if (newSolutions.size() > 1)
                return;
            });
      }
    };

    bool triedSemanticPreserve = false;

    // First try structure-preserving slot rewrites. Only if those do not
    // yield a unique rebuilt spelling do we consider flattening fallback.
    enumerateSlotPieces([&](StringRef piece) {
      auto semanticCerts =
          collectSlotSemanticRewriteCertificates(slot, wrapper, piece);
      if (semanticCerts.empty())
        return;

      triedSemanticPreserve = true;
      for (const auto &semanticCert : semanticCerts) {
        newParts.push_back(semanticCert.decision);
        newPartCertificates.push_back(semanticCert);
        self(self, idx + 1, pos + piece.size());
        newPartCertificates.pop_back();
        newParts.pop_back();

        if (newSolutions.size() > 1)
          return;
      }
    });

    // If semantic preservation already produced one or more solutions
    // that all rebuild to the same parent argument, do not explore the
    // weaker flattening fallback.
    if (triedSemanticPreserve && solutionsCollapseToSameRebuilt())
      return;

    enumerateSlotPieces([&](StringRef piece) {
      // Passthrough flattening is the explicit fallback: it keeps the new
      // observed text but records that no child syntax was preserved.
      SlotSemanticRewriteCertificate fallbackCert;
      fallbackCert.kind = SlotSemanticRewriteCertificateKind::Unique;
      fallbackCert.decision =
          SlotRewriteDecision{SlotRewriteDecisionKind::PassthroughFlatten,
                              piece.str(), piece.str()};
      fallbackCert.wrapperKind = wrapper.kind;
      fallbackCert.wrapperSource = wrapper.source;
      fallbackCert.logicalInputText = wrapper.logicalInputText;

      newParts.push_back(fallbackCert.decision);
      newPartCertificates.push_back(fallbackCert);
      self(self, idx + 1, pos + piece.size());
      newPartCertificates.pop_back();
      newParts.pop_back();
    });
  };

  solveNew(solveNew, 0, 0);

  if (newSolutions.empty()) {
    argCert.detail = "new arg did not admit any structurally valid slot "
                     "reconstruction";
    return argCert;
  }

  // Different slot decisions are still acceptable if they reconstruct the
  // same final argument text. Different final text means the rewrite is
  // ambiguous.
  std::string rebuilt = rebuildFromSolution(newSolutions[0]);
  for (size_t i = 1; i < newSolutions.size(); ++i) {
    if (rebuildFromSolution(newSolutions[i]) != rebuilt) {
      argCert.detail = "new arg admitted multiple non-equivalent "
                       "structural reconstructions";
      return argCert;
    }
  }

  argCert.slotDecisions.assign(newSolutions[0].begin(), newSolutions[0].end());
  if (!newSolutionCertificates.empty()) {
    argCert.slotCertificates.assign(newSolutionCertificates[0].begin(),
                                    newSolutionCertificates[0].end());
  }

  argCert.rawArgNewText = rebuilt;
  argCert.kind = (StringRef(argCert.rawArgNewText).trim() ==
                  StringRef(argCert.rawArgOldText).trim())
                     ? ArgSemanticRewriteCertificateKind::NoChange
                     : ArgSemanticRewriteCertificateKind::Unique;
  return argCert;
}

ArgSemanticRewriteCertificate
RefoldMacroDAGInvertibilitySolver::BuildObservedArgRewriteCertificate(
    const RefoldModel::MacroInvocation &parent, uint32_t parentFormal,
    StringRef observedOld0, StringRef observedNew0,
    const DenseMap<uint64_t, std::string> *preferredChildSyntax) const {
  auto invertibilityCert =
      BuildArgInvertibilityCertificate(parent, parentFormal, observedOld0);
  if (!invertibilityCert) {
    ArgSemanticRewriteCertificate argCert;
    argCert.failure = ArgSemanticRewriteFailure::MissingStructuralTemplate;
    argCert.detail = "observed old arg text did not match a unique "
                     "structural template";
    return argCert;
  }
  return BuildArgSemanticRewriteCertificate(*invertibilityCert, observedNew0,
                                            preferredChildSyntax);
}

std::string RefoldMacroDAGInvertibilitySolver::FormatFormalTextPairMap(
    const DenseMap<uint32_t, FormalTextPair> &formals) const {
  SmallVector<uint32_t, 8> argIdxs;
  argIdxs.reserve(formals.size());
  for (const auto &kvLocal : formals)
    argIdxs.push_back(kvLocal.first);
  llvm::sort(argIdxs);

  std::string out;
  raw_string_ostream os(out);
  os << "{";
  for (size_t i = 0; i < argIdxs.size(); ++i) {
    if (i)
      os << ", ";
    const uint32_t argIdx = argIdxs[i];
    const auto it = formals.find(argIdx);
    os << argIdx << ":'" << stringutils::showWsWithClip(it->second.oldText, 80)
       << "'->'" << stringutils::showWsWithClip(it->second.newText, 80) << "'";
  }
  os << "}";
  return os.str();
}

RawFormalValidationCertificate
RefoldMacroDAGInvertibilitySolver::BuildRawFormalValidationCertificate(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
    StringRef oldText0, StringRef newText0, StringRef traceStage,
    ArrayRef<diffutils::Hunk> tokenHunksAR,
    bool skipOccurrenceConsistency) const {
  RawFormalValidationCertificate cert;
  cert.inv = &inv;
  cert.argIdx = argIdx;
  cert.oldText = oldText0.trim().str();
  cert.newText = newText0.trim().str();

  const StringRef oldText = StringRef(cert.oldText).trim();
  const StringRef newText = StringRef(cert.newText).trim();

  // No text change means the replacement is trivially valid; there is no
  // need to run arity or occurrence checks.
  if (oldText == newText) {
    cert.valid = true;
    return cert;
  }

  // Non-variadic formals cannot receive a top-level comma, because that
  // would change the macro call's argument structure rather than only
  // replacing this formal's payload.
  if (!isMacroInvocationVariadicFormal(inv, argIdx) &&
      refoldMacroActualHasTopLevelComma(newText, deps_.lexLang)) {
    cert.failure = RawFormalValidationFailure::ArityChange;
    cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} arity "
                          "safety failed",
                          traceStage, inv.id, inv.name, argIdx)
                      .str();
    return cert;
  }

  // Some semantic rewrite paths validate occurrence consistency through a
  // stronger structural certificate. In those cases, this raw-text
  // validator only performs local arity/surface checks and records the
  // deferral.
  if (skipOccurrenceConsistency) {
    cert.valid = true;
    cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} occurrence "
                          "consistency deferred to semantic certificate "
                          "pipeline",
                          traceStage, inv.id, inv.name, argIdx)
                      .str();
    return cert;
  }

  // Require the proposed old->new formal rewrite to explain all non-paste
  // occurrences of this formal in the B stream. Paste-specific semantic
  // proof is intentionally ignored here because it is handled by
  // dedicated paste replay paths.
  if (!deps_
           .macroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
               inv, argIdx, oldText, newText, tokenHunksAR)) {
    cert.failure = RawFormalValidationFailure::OccurrenceMismatch;
    cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} occurrence "
                          "consistency failed",
                          traceStage, inv.id, inv.name, argIdx)
                      .str();
    return cert;
  }

  cert.valid = true;
  return cert;
}

PasteRewriteValidationCertificate
RefoldMacroDAGInvertibilitySolver::BuildPasteRewriteValidationCertificate(
    const RefoldModel::MacroInvocation &inv,
    const DenseMap<uint32_t, std::string> &replacementByArgIdx,
    StringRef traceStage, std::optional<StringRef> callsiteTextOverride,
    ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride) const {
  PasteRewriteValidationCertificate cert;
  cert.inv = &inv;
  cert.replacementByArgIdx = replacementByArgIdx;

  // First determine whether this certificate is even needed. If none of
  // the replaced formals participate in paste spans, paste replay cannot
  // constrain the candidate rewrite.
  for (const auto &kv : replacementByArgIdx) {
    const uint32_t argIdx = kv.first;
    for (const auto &ps : inv.pasteSpans) {
      if (ps.argIdx == argIdx) {
        cert.required = true;
        break;
      }
    }
    if (cert.required)
      break;
  }

  if (!cert.required)
    return cert;

  StringRef callsiteText;
  ArrayRef<std::pair<size_t, size_t>> callsiteArgRanges;
  std::optional<SmallVector<std::pair<size_t, size_t>, 8>> ownedArgRanges;

  if (callsiteTextOverride) {
    // Some callers validate against a freshly rewritten invocation
    // surface. In that mode, the caller must also provide argument ranges
    // relative to the override text; the producer's original ranges no
    // longer apply.
    callsiteText = *callsiteTextOverride;
    if (callsiteArgRangesOverride.empty()) {
      cert.valid = false;
      cert.failure = PasteRewriteValidationFailure::MissingArgumentRanges;
      cert.detail = formatv("{0}: paste consistency unavailable: inv id={1} "
                            "name={2} arg ranges unavailable",
                            traceStage, inv.id, inv.name)
                        .str();
      return cert;
    }
    callsiteArgRanges = callsiteArgRangesOverride;
  } else {
    // Default mode validates against the invocation spelling recorded by
    // the producer and derives one argument range per formal from that
    // spelling.
    if (!inv.invText) {
      cert.valid = false;
      cert.failure = PasteRewriteValidationFailure::MissingInvocationText;
      cert.detail = formatv("{0}: paste consistency unavailable: inv id={1} "
                            "name={2} hasInvText=0",
                            traceStage, inv.id, inv.name)
                        .str();
      return cert;
    }

    callsiteText = StringRef(*inv.invText);
    auto invArgRangesOpt =
        deps_.getMacroInvocationFormalArgContentRanges(inv, callsiteText);
    if (!invArgRangesOpt) {
      cert.valid = false;
      cert.failure = PasteRewriteValidationFailure::MissingArgumentRanges;
      cert.detail = formatv("{0}: paste consistency unavailable: inv id={1} "
                            "name={2} arg ranges unavailable",
                            traceStage, inv.id, inv.name)
                        .str();
      return cert;
    }

    // Keep the derived ranges alive while exposing them through ArrayRef
    // below.
    ownedArgRanges.emplace(invArgRangesOpt->begin(), invArgRangesOpt->end());
    callsiteArgRanges = *ownedArgRanges;
  }

  // The decisive paste check: after applying all proposed argument
  // replacements, every paste token produced by this invocation must
  // match the corresponding B-side pasted token spelling.
  if (!pasteArgumentBuilder().PasteArgReplacementsMatchAllPasteTokensInB(
          inv, callsiteText, callsiteArgRanges, cert.replacementByArgIdx)) {
    cert.valid = false;
    cert.failure = PasteRewriteValidationFailure::PasteMismatch;
    cert.detail =
        formatv("{0}: paste-token consistency failed: inv id={1} "
                "name={2} touchedArgs={3}",
                traceStage, inv.id, inv.name, cert.replacementByArgIdx.size())
            .str();
    return cert;
  }

  return cert;
}

SemanticInteractionCertificate
RefoldMacroDAGInvertibilitySolver::BuildSemanticInteractionCertificate(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
    const ArgSemanticRewriteCertificate &argCert, StringRef traceStage) const {
  SemanticInteractionCertificate cert;
  cert.inv = &inv;
  cert.argIdx = argIdx;
  cert.slotCertificates.assign(argCert.slotCertificates.begin(),
                               argCert.slotCertificates.end());

  auto addLogicalInput = [&](StringRef logical0) {
    // Keep a unique list of canonical logical child inputs represented by
    // the slot certificates. This is diagnostic/proof metadata, not
    // replacement text.
    const std::string normalized = logical0.trim().str();
    if (normalized.empty())
      return;
    for (const auto &existing : cert.canonicalLogicalInputs)
      if (existing == normalized)
        return;
    cert.canonicalLogicalInputs.push_back(normalized);
  };

  // Paste participation is a property of the invocation/formal as a
  // whole, not of any one wrapper slot.
  cert.touchesPaste = invocationArgTouchesPaste(inv, argIdx);

  for (const auto &slotCert : argCert.slotCertificates) {
    // Record how each child slot was rebuilt: preferred new child syntax,
    // preserved raw child invocation spelling, or flattened observed
    // text.
    switch (slotCert.decision.kind) {
    case SlotRewriteDecisionKind::PreferredChildSyntax:
      cert.usesPreferredChildSyntax = true;
      break;
    case SlotRewriteDecisionKind::PreserveRawInvocation:
      cert.usesRawInvocationPreservation = true;
      break;
    case SlotRewriteDecisionKind::PassthroughFlatten:
      cert.usesPassthroughFlatten = true;
      break;
    }

    if (slotCert.wrapperSource == WrapperObservedSource::ChildRawInvocation)
      cert.usesRawChildInvocationLogicalInput = true;

    // Exact wrappers contribute their logical input directly. Stringified
    // wrappers must first prove that their inverse payload is canonical,
    // so later paste/stringify interaction checks do not depend on
    // ambiguous escape spellings.
    switch (slotCert.wrapperKind) {
    case WrapperChainKind::Exact:
      addLogicalInput(slotCert.logicalInputText);
      break;
    case WrapperChainKind::StringLiteral:
    case WrapperChainKind::WideStringLiteral: {
      cert.usesStringify = true;
      if (slotCert.wrapperKind == WrapperChainKind::WideStringLiteral)
        cert.usesWideStringify = true;

      auto canon = stringutils::canonicalizeStringifyInversePayload(
          slotCert.logicalInputText);
      if (!canon || StringRef(*canon).trim() !=
                        StringRef(slotCert.logicalInputText).trim()) {
        cert.valid = false;
        cert.failure = SemanticInteractionFailure::NonCanonicalLogicalInput;
        cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} "
                              "interaction canonical logical payload "
                              "failed",
                              traceStage, inv.id, inv.name, argIdx)
                          .str();
        return cert;
      }

      addLogicalInput(*canon);
      break;
    }
    }
  }

  const bool hasChildSyntax =
      cert.usesPreferredChildSyntax || cert.usesRawInvocationPreservation;
  const bool hasStringify = cert.usesStringify;
  const bool hasPaste = cert.touchesPaste;

  // Classify the interaction from most constrained combinations to
  // simpler single-feature cases. Paste+stringify and paste+child-syntax
  // combinations are more specific than plain paste/stringify because
  // they require additional cross-feature validation.
  if (hasPaste && hasStringify && cert.usesWideStringify) {
    cert.kind = SemanticInteractionKind::WideStringifyPaste;
  } else if (hasPaste && hasStringify) {
    cert.kind = SemanticInteractionKind::StringifyPaste;
  } else if (hasPaste && cert.usesRawInvocationPreservation) {
    cert.kind = SemanticInteractionKind::RawInvocationPaste;
  } else if (hasPaste && cert.usesPreferredChildSyntax) {
    cert.kind = SemanticInteractionKind::ChildSyntaxPaste;
  } else if (hasPaste) {
    cert.kind = SemanticInteractionKind::Paste;
  } else if (hasStringify && cert.usesWideStringify) {
    cert.kind = SemanticInteractionKind::WideStringify;
  } else if (hasStringify) {
    cert.kind = SemanticInteractionKind::Stringify;
  } else if (cert.usesRawInvocationPreservation) {
    cert.kind = SemanticInteractionKind::RawInvocation;
  } else if (cert.usesPreferredChildSyntax) {
    cert.kind = SemanticInteractionKind::ChildSyntax;
  } else {
    cert.kind = SemanticInteractionKind::Plain;
  }

  // If all three major mechanisms interact, keep the coarser Mixed
  // category so downstream code does not accidentally treat it as one of
  // the simpler two-feature cases.
  if (hasChildSyntax && hasStringify && hasPaste)
    cert.kind = SemanticInteractionKind::Mixed;

  cert.detail =
      formatv("semantic interaction: inv id={0} name={1} argIdx={2}"
              " kind={3} slots={4} logicalInputs={5} paste={6} "
              "rawChildInput={7}",
              inv.id, inv.name, argIdx, static_cast<unsigned>(cert.kind),
              cert.slotCertificates.size(), cert.canonicalLogicalInputs.size(),
              cert.touchesPaste ? 1 : 0,
              cert.usesRawChildInvocationLogicalInput ? 1 : 0)
          .str();
  return cert;
}

SemanticInteractionSignature
RefoldMacroDAGInvertibilitySolver::BuildSemanticInteractionSignature(
    const SemanticInteractionCertificate &interaction) const {
  SemanticInteractionSignature sig;
  sig.touchesPaste = interaction.touchesPaste;
  sig.usesPreferredChildSyntax = interaction.usesPreferredChildSyntax;
  sig.usesRawInvocationPreservation = interaction.usesRawInvocationPreservation;
  sig.usesPassthroughFlatten = interaction.usesPassthroughFlatten;
  sig.usesStringify = interaction.usesStringify;
  sig.usesWideStringify = interaction.usesWideStringify;
  sig.usesRawChildInvocationLogicalInput =
      interaction.usesRawChildInvocationLogicalInput;
  sig.canonicalLogicalInputs.assign(interaction.canonicalLogicalInputs.begin(),
                                    interaction.canonicalLogicalInputs.end());
  llvm::sort(sig.canonicalLogicalInputs);
  sig.canonicalLogicalInputs.erase(
      std::unique(sig.canonicalLogicalInputs.begin(),
                  sig.canonicalLogicalInputs.end()),
      sig.canonicalLogicalInputs.end());
  return sig;
}

FormalInteractionConsistencyCertificate
RefoldMacroDAGInvertibilitySolver::BuildFormalInteractionConsistencyCertificate(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
    ArrayRef<SemanticInteractionCertificate> interactions,
    StringRef traceStage) const {
  FormalInteractionConsistencyCertificate cert;
  cert.inv = &inv;
  cert.argIdx = argIdx;
  cert.interactions.assign(interactions.begin(), interactions.end());

  // No observations means there is no conflicting semantic evidence for
  // this formal. Treat that as a vacuous success.
  if (interactions.empty()) {
    cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} semantic "
                          "interaction convergence vacuously satisfied",
                          traceStage, inv.id, inv.name, argIdx)
                      .str();
    return cert;
  }

  // Use the first observation as the required interaction signature, then
  // demand exact signature equality for all remaining observations of the
  // same formal.
  cert.signature = BuildSemanticInteractionSignature(interactions.front());
  for (size_t i = 1; i < interactions.size(); ++i) {
    auto sig = BuildSemanticInteractionSignature(interactions[i]);
    if (!(sig == cert.signature)) {
      cert.valid = false;
      cert.failure =
          FormalInteractionConsistencyFailure::DivergentSemanticEvidence;
      cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} semantic "
                            "interaction evidence diverged across "
                            "observations",
                            traceStage, inv.id, inv.name, argIdx)
                        .str();
      return cert;
    }
  }

  cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} semantic "
                        "interaction convergence satisfied "
                        "logicalInputs={4} paste={5} stringify={6} wide={7} "
                        "rawInvocation={8} childSyntax={9} passthrough={10}",
                        traceStage, inv.id, inv.name, argIdx,
                        cert.signature.canonicalLogicalInputs.size(),
                        cert.signature.touchesPaste ? 1 : 0,
                        cert.signature.usesStringify ? 1 : 0,
                        cert.signature.usesWideStringify ? 1 : 0,
                        cert.signature.usesRawInvocationPreservation ? 1 : 0,
                        cert.signature.usesPreferredChildSyntax ? 1 : 0,
                        cert.signature.usesPassthroughFlatten ? 1 : 0)
                    .str();
  return cert;
}

FormalRewriteCertificate
RefoldMacroDAGInvertibilitySolver::BuildObservedFormalRewriteCertificate(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
    ArrayRef<ObservedFormalConstraint> observedConstraints,
    const DenseMap<uint64_t, std::string> *preferredChildSyntax,
    StringRef traceStage, ArrayRef<diffutils::Hunk> tokenHunksAR) const {
  FormalRewriteCertificate cert;
  cert.inv = &inv;
  cert.argIdx = argIdx;

  // Start from the original call-site argument spelling. All candidate
  // rewrites for this formal are merged relative to this same old text.
  auto argText = deps_.textPrimitives.GetInvocationArgText(inv, argIdx);
  if (!argText) {
    cert.failure = FormalRewriteFailure::MissingArgumentText;
    cert.detail =
        formatv("{0}: inv id={1} name={2} argIdx={3} text unavailable",
                traceStage, inv.id, inv.name, argIdx)
            .str();
    return cert;
  }

  const StringRef oldTrim = argText->trim();

  for (const auto &constraint : observedConstraints) {
    // Convert each observed expansion-level old/new pair into an
    // argument-level semantic rewrite certificate. This is where
    // child-slot/template evidence is used to map observed text back to
    // the parent formal.
    auto argRewriteCert = BuildObservedArgRewriteCertificate(
        inv, argIdx, constraint.oldText, constraint.newText,
        preferredChildSyntax);
    if (argRewriteCert.kind == ArgSemanticRewriteCertificateKind::Invalid) {
      cert.failure =
          argRewriteCert.failure ==
                  ArgSemanticRewriteFailure::MissingStructuralTemplate
              ? FormalRewriteFailure::MissingStructuralTemplate
              : FormalRewriteFailure::RawRewriteNotCertifiable;
      cert.detail =
          formatv("{0}: inv id={1} name={2} argIdx={3} raw "
                  "rewrite not certifiable ({4})",
                  traceStage, inv.id, inv.name, argIdx, argRewriteCert.detail)
              .str();
      return cert;
    }

    // Classify the semantic mechanisms involved in this observation
    // (child-syntax preservation, raw invocation preservation, stringify,
    // paste, flattening). Later all observations for the same formal
    // must agree on this interaction shape.
    auto interactionCert = BuildSemanticInteractionCertificate(
        inv, argIdx, argRewriteCert, traceStage);
    if (!interactionCert.valid) {
      cert.failure = FormalRewriteFailure::RawRewriteNotCertifiable;
      cert.detail = interactionCert.detail;
      return cert;
    }

    cert.argRewriteCertificates.push_back(argRewriteCert);
    cert.interactionCertificates.push_back(interactionCert);

    // Store the concrete old->new formal-text rewrite proposed by this
    // observation. The merge step below will require all observations to
    // be compatible with one final replacement.
    cert.candidateRewrites.push_back(FormalTextPair{
        oldTrim.str(), StringRef(argRewriteCert.rawArgNewText).trim().str()});
  }

  // All observations of this formal must use the same semantic proof
  // shape. A mix such as one occurrence requiring paste+stringify and
  // another requiring plain child syntax is not treated as one coherent
  // formal rewrite.
  cert.interactionConsistency = BuildFormalInteractionConsistencyCertificate(
      inv, argIdx, cert.interactionCertificates, traceStage);
  if (!cert.interactionConsistency.valid) {
    cert.failure = FormalRewriteFailure::InteractionConflict;
    cert.detail = cert.interactionConsistency.detail;
    return cert;
  }

  // Collapse all observation-level candidate rewrites into one
  // replacement for the formal. Conflicting replacements fail closed
  // instead of picking one.
  auto merged = MergeCompatibleFormalRewrites(oldTrim, cert.candidateRewrites);
  if (!merged) {
    cert.failure = FormalRewriteFailure::MergeConflict;
    cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} rewrite merge "
                          "conflicted",
                          traceStage, inv.id, inv.name, argIdx)
                      .str();
    return cert;
  }

  const StringRef mergedTrim = StringRef(*merged).trim();
  cert.oldText = oldTrim.str();
  cert.newText = mergedTrim.str();

  // A fully certified rewrite can still collapse to no change after
  // merging.
  if (mergedTrim == oldTrim) {
    cert.kind = FormalRewriteCertificateKind::NoChange;
    cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} certified "
                          "rewrite collapsed to no-change old='{4}' new='{5}'"
                          " argRewriteCerts={6} interactionCerts={7}",
                          traceStage, inv.id, inv.name, argIdx, oldTrim,
                          mergedTrim, cert.argRewriteCertificates.size(),
                          cert.interactionCertificates.size())
                      .str();
    return cert;
  }

  // If a stronger semantic pipeline already validated the occurrence
  // behavior for child syntax, stringify, or paste interactions, the
  // raw-text occurrence check is deferred to that certificate. Plain
  // rewrites still use the direct all-occurrences validation.
  const bool deferOccurrenceConsistency =
      cert.interactionConsistency.signature.usesPreferredChildSyntax ||
      cert.interactionConsistency.signature.usesRawInvocationPreservation ||
      cert.interactionConsistency.signature.usesStringify ||
      cert.interactionConsistency.signature.usesWideStringify ||
      cert.interactionConsistency.signature.touchesPaste;

  cert.validation = BuildRawFormalValidationCertificate(
      inv, argIdx, oldTrim, mergedTrim, traceStage, tokenHunksAR,
      deferOccurrenceConsistency);
  if (!cert.validation.valid) {
    switch (cert.validation.failure) {
    case RawFormalValidationFailure::ArityChange:
      cert.failure = FormalRewriteFailure::ArityChange;
      break;
    case RawFormalValidationFailure::OccurrenceMismatch:
      cert.failure = FormalRewriteFailure::OccurrenceMismatch;
      break;
    case RawFormalValidationFailure::None:
      cert.failure = FormalRewriteFailure::None;
      break;
    }
    cert.detail = cert.validation.detail;
    return cert;
  }

  cert.kind = FormalRewriteCertificateKind::Unique;
  return cert;
}

std::optional<std::string>
RefoldMacroDAGInvertibilitySolver::MergeCompatibleFormalRewrites(
    StringRef baseOld0, ArrayRef<FormalTextPair> rewrites) const {
  const StringRef baseOld = baseOld0.trim();
  std::vector<std::string> replacementStorage;
  replacementStorage.reserve(rewrites.size());

  for (const auto &rewrite : rewrites) {
    if (StringRef(rewrite.oldText).trim() != baseOld)
      return std::nullopt;
    replacementStorage.push_back(StringRef(rewrite.newText).trim().str());
  }

  SmallVector<StringRef, 8> replacementRefs;
  replacementRefs.reserve(replacementStorage.size());
  for (const std::string &replacement : replacementStorage)
    replacementRefs.push_back(StringRef(replacement));

  return mergeCompatibleStringReplacements(baseOld, replacementRefs);
}

} // namespace refold
} // namespace clang
