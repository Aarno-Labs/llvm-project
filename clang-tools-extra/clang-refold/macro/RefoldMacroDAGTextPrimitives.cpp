//===--- RefoldMacroDAGTextPrimitives.cpp -----------------------*- C++ -*-===//
//
// Text and token primitive service for macro DAG lifting.
//
// The routines in this translation unit recover formal-argument text, build
// observed forms, normalize stringification/paste surfaces, and compare the
// source-level spellings consumed by the higher DAG proof stages.  The service
// borrows only the lexical/model dependencies it needs and exposes
// deterministic helpers to the invertibility, structured-lift, subtree, and
// candidate validation services.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDAGTextPrimitives.h"

#include "core/RefoldModel.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroDAGTextPrimitives::RefoldMacroDAGTextPrimitives(Dependencies deps)
    : deps_(std::move(deps)) {}

std::optional<StringRef> RefoldMacroDAGTextPrimitives::GetInvocationArgText(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx) const {
  if (!inv.invText)
    return std::nullopt;
  auto rangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(inv, *inv.invText);
  if (!rangesOpt || argIdx >= rangesOpt->size())
    return std::nullopt;
  const auto &rng = (*rangesOpt)[argIdx];
  if (rng.second < rng.first || rng.second > inv.invText->size())
    return std::nullopt;
  return StringRef(*inv.invText)
      .slice((size_t)rng.first, (size_t)rng.second)
      .trim();
}

std::optional<ArgRefTemplate> RefoldMacroDAGTextPrimitives::BuildArgRefTemplate(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx) const {
  if (!inv.invText || !inv.invB)
    return std::nullopt;
  if (argIdx >= inv.invArgRanges.size() || argIdx >= inv.argRefs.size())
    return std::nullopt;

  const auto &rng = inv.invArgRanges[argIdx];
  if (!rng.first || !rng.second || *rng.second < *rng.first ||
      *rng.first < *inv.invB)
    return std::nullopt;

  const uint64_t relB = *rng.first - *inv.invB;
  const uint64_t relE = *rng.second - *inv.invB;
  if (relE < relB || relE > inv.invText->size())
    return std::nullopt;

  StringRef rawArg = StringRef(*inv.invText).slice((size_t)relB, (size_t)relE);

  size_t trimLead = 0;
  size_t trimEnd = rawArg.size();
  std::tie(trimLead, trimEnd) =
      stringutils::trimWsRange(rawArg, 0, rawArg.size());

  ArgRefTemplate out;
  out.argText = rawArg.slice(trimLead, trimEnd).str();

  SmallVector<LocalArgRef, 4> refs;
  refs.reserve(inv.argRefs[argIdx].size());

  for (const auto &ref : inv.argRefs[argIdx]) {
    if (ref.byteEnd < ref.byteBegin)
      return std::nullopt;
    if (ref.byteBegin < relB || ref.byteEnd > relE)
      return std::nullopt;

    const uint64_t localBAbs = ref.byteBegin - relB;
    const uint64_t localEAbs = ref.byteEnd - relB;
    if (localEAbs < localBAbs || localEAbs > rawArg.size())
      return std::nullopt;

    if (localBAbs < trimLead || localEAbs > trimEnd)
      return std::nullopt;

    refs.push_back(LocalArgRef{ref.callerParamIndex,
                               (uint32_t)(localBAbs - trimLead),
                               (uint32_t)(localEAbs - trimLead)});
  }

  llvm::sort(refs, [](const LocalArgRef &a, const LocalArgRef &b) {
    if (a.begin != b.begin)
      return a.begin < b.begin;
    if (a.end != b.end)
      return a.end < b.end;
    return a.callerParamIndex < b.callerParamIndex;
  });

  uint32_t prevEnd = 0;
  bool first = true;
  for (const auto &ref : refs) {
    if (ref.end < ref.begin || ref.end > out.argText.size())
      return std::nullopt;
    if (!first && ref.begin < prevEnd)
      return std::nullopt;

    prevEnd = ref.end;
    first = false;

    if (llvm::find(out.distinctCallerParams, ref.callerParamIndex) ==
        out.distinctCallerParams.end())
      out.distinctCallerParams.push_back(ref.callerParamIndex);
  }

  out.refs = std::move(refs);
  return out;
}

std::optional<TrimmedArgInfo>
RefoldMacroDAGTextPrimitives::GetTrimmedInvocationArgInfo(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx) const {
  if (!inv.invText || !inv.invB)
    return std::nullopt;
  auto rangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(inv, *inv.invText);
  if (!rangesOpt || argIdx >= rangesOpt->size())
    return std::nullopt;

  const auto &rng = (*rangesOpt)[argIdx];
  if (rng.second < rng.first || rng.second > inv.invText->size())
    return std::nullopt;

  StringRef raw =
      StringRef(*inv.invText).slice((size_t)rng.first, (size_t)rng.second);

  size_t trimLead = 0;
  size_t trimEnd = raw.size();
  std::tie(trimLead, trimEnd) = stringutils::trimWsRange(raw, 0, raw.size());

  TrimmedArgInfo out;
  out.text = raw.slice(trimLead, trimEnd).str();
  out.absTrimBegin = *inv.invB + rng.first + trimLead;
  out.absTrimEnd = *inv.invB + rng.first + trimEnd;
  return out;
}

std::optional<std::string>
RefoldMacroDAGTextPrimitives::GetInvocationCoverAText(
    const RefoldModel::MacroInvocation &inv) const {
  uint64_t covLoA = inv.cover.begin;
  uint64_t covHiA = inv.cover.end;
  if (inv.subkind == "func" && inv.defParams.empty() &&
      !inv.bodySpans.empty()) {
    uint64_t lo = std::numeric_limits<uint64_t>::max();
    uint64_t hi = 0;
    for (const auto &s : inv.bodySpans) {
      if (s.begin < s.end) {
        lo = std::min(lo, s.begin);
        hi = std::max(hi, s.end);
      }
    }
    if (lo != std::numeric_limits<uint64_t>::max() && lo < hi) {
      covLoA = lo;
      covHiA = hi;
    }
  }
  if (covLoA >= covHiA)
    return std::nullopt;
  return deps_.sourceMapper.SliceASource(covLoA, covHiA).trim().str();
}

bool RefoldMacroDAGTextPrimitives::IsLikelyTokenBoundaryInRefoldText(
    StringRef s, size_t pos) const {
  if (pos == 0 || pos >= s.size())
    return true;
  return !(stringutils::isIdentPart(s[pos - 1]) &&
           stringutils::isIdentPart(s[pos]));
}

bool RefoldMacroDAGTextPrimitives::IsBalancedRefoldFragment(StringRef s) const {
  bool balancedAtEnd = false;
  enumerateTopLevelBalancedCutPointsWithLexer(s, deps_.lexLang,
                                              [&](unsigned cut) {
                                                if (cut == s.size())
                                                  balancedAtEnd = true;
                                              });
  return balancedAtEnd;
}

void RefoldMacroDAGTextPrimitives::EnumerateTopLevelLiteralMatchesInRefoldText(
    StringRef haystack, StringRef needle, size_t maxPos,
    const std::function<void(size_t)> &emitMatch) const {
  if (needle.empty())
    return;
  enumerateTopLevelBalancedCutPointsWithLexer(
      haystack, deps_.lexLang, [&](unsigned cut) {
        const size_t pos = static_cast<size_t>(cut);
        if (pos > maxPos)
          return;
        if (!IsLikelyTokenBoundaryInRefoldText(haystack, pos))
          return;
        if (haystack.drop_front(pos).starts_with(needle))
          emitMatch(pos);
      });
}

std::optional<std::string>
RefoldMacroDAGTextPrimitives::BuildRewrittenInvocationSyntax(
    const RefoldModel::MacroInvocation &inv,
    const DenseMap<uint32_t, std::string> &replByFormal) const {
  if (!inv.invText || !inv.invB)
    return std::nullopt;
  auto rangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(inv, *inv.invText);
  if (!rangesOpt)
    return std::nullopt;

  struct LocalEdit {
    uint64_t begin = 0;
    uint64_t end = 0;
    std::string repl;
  };

  SmallVector<LocalEdit, 8> edits;
  edits.reserve(replByFormal.size());
  for (const auto &kvLocal : replByFormal) {
    const uint32_t argIdx = kvLocal.first;
    if (argIdx >= rangesOpt->size())
      return std::nullopt;
    const auto &rng = (*rangesOpt)[argIdx];
    if (rng.second < rng.first || rng.second > inv.invText->size())
      return std::nullopt;

    const uint64_t relB = rng.first;
    const uint64_t relE = rng.second;

    StringRef newArg = StringRef(kvLocal.second).trim();
    const bool allowComma =
        argIdx < inv.defParams.size() && inv.defParams[argIdx].variadic;
    if (!allowComma && refoldMacroActualHasTopLevelComma(newArg, deps_.lexLang))
      return std::nullopt;

    edits.push_back(LocalEdit{relB, relE, newArg.str()});
  }

  llvm::sort(edits, [](const LocalEdit &a, const LocalEdit &b) {
    return a.begin > b.begin;
  });

  std::string rewritten = inv.invText->str();
  for (const auto &edit : edits)
    rewritten =
        stringutils::replaceRange(rewritten, edit.begin, edit.end, edit.repl);
  return StringRef(rewritten).trim().str();
}

SmallVector<std::string, 4>
RefoldMacroDAGTextPrimitives::GetExpansionTextCandidates(
    const RefoldModel::MacroInvocation &inv, bool fromB) const {
  SmallVector<std::string, 4> out;
  std::optional<std::string> base =
      fromB ? deps_.proofLattice.BuildWholeCoverReplacementText(inv)
            : GetInvocationCoverAText(inv);
  if (!base)
    return out;

  auto addUnique = [&](StringRef s) {
    std::string cand = s.trim().str();
    if (cand.empty())
      return;
    if (llvm::find(out, cand) == out.end())
      out.push_back(std::move(cand));
  };

  auto tryAddUnstringified = [&](StringRef raw) {
    auto un = deps_.argTextRecovery.UnstringifyLiteralToArgText(
        raw, /*allowTopLevelComma=*/true);
    if (!un)
      return;
    addUnique(*un);
  };

  auto tryAddWideLiteral = [&](StringRef raw) {
    StringRef t = raw.trim();
    if (!stringutils::looksLikeStringLiteralToken(t))
      return;

    if (t.starts_with("L\"") || t.starts_with("u\"") || t.starts_with("U\"") ||
        t.starts_with("u8\""))
      return;

    addUnique((Twine("L") + t).str());
  };

  addUnique(*base);
  tryAddUnstringified(*base);
  tryAddWideLiteral(*base);
  return out;
}

SmallVector<LexicalChildPlaceholder, 4>
RefoldMacroDAGTextPrimitives::GetTopLevelLexicalChildrenInArg(
    const RefoldModel::MacroInvocation &parent, uint32_t parentFormal) const {
  SmallVector<LexicalChildPlaceholder, 8> cands;

  auto argInfo = GetTrimmedInvocationArgInfo(parent, parentFormal);
  if (!argInfo || !parent.invFile)
    return SmallVector<LexicalChildPlaceholder, 4>{};

  for (const auto &cand : deps_.model.GetMacroInvocations()) {
    if (cand.id == parent.id || !cand.invFile || !cand.invB || !cand.invE)
      continue;
    if (*cand.invFile != *parent.invFile)
      continue;
    if (*cand.invB < argInfo->absTrimBegin ||
        *cand.invE > argInfo->absTrimEnd || *cand.invE <= *cand.invB)
      continue;

    auto olds = GetExpansionTextCandidates(cand, /*fromB=*/false);
    auto news = GetExpansionTextCandidates(cand, /*fromB=*/true);

    SmallVector<WrapperChainCertificate, 4> forms;

    auto addObservedForm = [&](WrapperChainKind kind,
                               WrapperObservedSource source, StringRef text,
                               StringRef logicalInput) {
      std::string observed = text.trim().str();
      std::string logical = logicalInput.trim().str();
      if (observed.empty() || logical.empty())
        return;

      if (kind == WrapperChainKind::StringLiteral ||
          kind == WrapperChainKind::WideStringLiteral) {
        auto canon = stringutils::canonicalizeStringifyInversePayload(logical);
        if (!canon || StringRef(*canon).trim() != StringRef(logical).trim())
          return;
        logical = std::move(*canon);
      }

      for (const auto &existing : forms) {
        if (existing.kind == kind && existing.observedOldText == observed &&
            existing.logicalInputText == logical)
          return;
      }

      forms.push_back(WrapperChainCertificate{kind, source, std::move(observed),
                                              std::move(logical)});
    };

    for (StringRef oldText : olds) {
      StringRef trimmed = oldText.trim();
      addObservedForm(WrapperChainKind::Exact,
                      WrapperObservedSource::ChildExpansion, trimmed, trimmed);
      addObservedForm(WrapperChainKind::StringLiteral,
                      WrapperObservedSource::ChildExpansion,
                      stringutils::quoteCStringLiteral(trimmed), trimmed);
      addObservedForm(
          WrapperChainKind::WideStringLiteral,
          WrapperObservedSource::ChildExpansion,
          (Twine("L") + stringutils::quoteCStringLiteral(trimmed)).str(),
          trimmed);
    }

    if (cand.invText) {
      const std::string rawInvocation = StringRef(*cand.invText).trim().str();
      if (!rawInvocation.empty()) {
        addObservedForm(WrapperChainKind::Exact,
                        WrapperObservedSource::ChildRawInvocation,
                        rawInvocation, rawInvocation);
        addObservedForm(WrapperChainKind::StringLiteral,
                        WrapperObservedSource::ChildRawInvocation,
                        stringutils::quoteCStringLiteral(rawInvocation),
                        rawInvocation);
        addObservedForm(
            WrapperChainKind::WideStringLiteral,
            WrapperObservedSource::ChildRawInvocation,
            (Twine("L") + stringutils::quoteCStringLiteral(rawInvocation))
                .str(),
            rawInvocation);
      }
    }

    if (forms.empty())
      continue;

    LexicalChildPlaceholder ph;
    ph.relBegin = *cand.invB - argInfo->absTrimBegin;
    ph.relEnd = *cand.invE - argInfo->absTrimBegin;
    ph.child = &cand;
    ph.observedForms = std::move(forms);
    ph.newExpansionCandidates = std::move(news);
    if (cand.invText)
      ph.rawInvocationText = StringRef(*cand.invText).trim().str();

    cands.push_back(std::move(ph));
  }

  llvm::sort(cands, [](const LexicalChildPlaceholder &a,
                       const LexicalChildPlaceholder &b) {
    if (a.relBegin != b.relBegin)
      return a.relBegin < b.relBegin;
    return a.relEnd > b.relEnd;
  });

  SmallVector<LexicalChildPlaceholder, 4> top;
  for (const auto &cand : cands) {
    bool contained = false;
    for (const auto &sel : top) {
      if (cand.relBegin >= sel.relBegin && cand.relEnd <= sel.relEnd) {
        contained = true;
        break;
      }

      if (!(cand.relEnd <= sel.relBegin || cand.relBegin >= sel.relEnd)) {
        contained = true;
        break;
      }
    }

    if (!contained)
      top.push_back(cand);
  }

  return top;
}

} // namespace refold
} // namespace clang
