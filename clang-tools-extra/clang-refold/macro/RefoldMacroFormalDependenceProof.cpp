//===--- RefoldMacroFormalDependenceProof.cpp -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of the producer-backed formal dependence proof.  See the
// header for the contract.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroFormalDependenceProof.h"

#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "source/TokenTextHelpers.h"
#include "support/RefoldLog.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <optional>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Append the formals of \p inv whose standard, stringified, or pasted
/// occurrence produced A token \p tok.
void appendFormalsObservedAtToken(const RefoldModel::MacroInvocation &inv,
                                  uint64_t tok,
                                  SmallVectorImpl<uint32_t> &formals) {
  for (const std::vector<RefoldModel::PPArgSpan> *spans :
       {&inv.argSpans, &inv.stringifySpans, &inv.pasteSpans})
    for (const RefoldModel::PPArgSpan &span : *spans)
      if (span.begin <= tok && tok < span.end)
        formals.push_back(span.argIdx);
}

/// Return whether one of \p spans contains A token \p tok.
bool spansContainToken(ArrayRef<RefoldModel::PPSpan> spans, uint64_t tok) {
  return llvm::any_of(spans, [tok](const RefoldModel::PPSpan &span) {
    return span.begin <= tok && tok < span.end;
  });
}

/// Return whether \p inv itself produced A token \p tok, by its replacement
/// list or by an occurrence of one of its formals.
///
/// This is narrower than `inv.spans`, which the producer may widen to an
/// envelope over tokens the invocation never produced.
bool invocationClaimsToken(const RefoldModel::MacroInvocation &inv,
                           uint64_t tok) {
  if (spansContainToken(inv.bodySpans, tok))
    return true;
  SmallVector<uint32_t, 4> formals;
  appendFormalsObservedAtToken(inv, tok, formals);
  return !formals.empty();
}

/// Return the caller formals referenced by the actual bound to formal
/// \p formal of \p inv, from the producer's edge-local `arg_deps`.
///
/// `arg_deps` is indexed by actual, so a variadic formal collects every actual
/// from its position on and an omitted variadic actual references nothing.
/// Returns nullopt when the producer recorded no `arg_deps` for \p inv, or no
/// entry for a non-variadic formal.
std::optional<SmallVector<uint32_t, 4>>
callerFormalsOfFormal(const RefoldModel::MacroInvocation &inv,
                      uint32_t formal) {
  if (inv.argDeps.empty())
    return std::nullopt;
  const bool variadic =
      formal < inv.defParams.size() && inv.defParams[formal].variadic;
  if (!variadic && formal >= inv.argDeps.size())
    return std::nullopt;
  SmallVector<uint32_t, 4> callerFormals;
  const size_t end = variadic ? inv.argDeps.size() : size_t(formal) + 1;
  for (size_t actual = formal; actual < end; ++actual)
    append_range(callerFormals, inv.argDeps[actual]);
  return callerFormals;
}

/// Return whether two invocations are spelled in one file of one include
/// instance, so their callsite byte ranges are comparable.
bool spelledInSameFileInstance(const RefoldModel::MacroInvocation &lhs,
                               const RefoldModel::MacroInvocation &rhs) {
  return lhs.invFile && rhs.invFile && *lhs.invFile == *rhs.invFile &&
         lhs.ownerIncludeId == rhs.ownerIncludeId;
}

/// Return whether the half-open callsite spelling of \p inner lies inside
/// `[begin, end)` of the same file instance as \p outer.
bool callsiteInside(const RefoldModel::MacroInvocation &outer,
                    const RefoldModel::MacroInvocation &inner,
                    std::optional<uint64_t> begin,
                    std::optional<uint64_t> end) {
  return spelledInSameFileInstance(outer, inner) && begin && end &&
         inner.invB && inner.invE && *begin <= *inner.invB &&
         *inner.invE <= *end;
}

/// Return whether two spellings lex to the same token sequence.
bool tokenEquivalent(StringRef lhs, StringRef rhs,
                     const clang::LangOptions &lexLang) {
  SmallVector<RefoldLexBoundaryToken, 16> lhsToks;
  SmallVector<RefoldLexBoundaryToken, 16> rhsToks;
  refoldLexBoundaryTokens(lhs, lexLang, lhsToks);
  refoldLexBoundaryTokens(rhs, lexLang, rhsToks);
  return llvm::equal(
      lhsToks, rhsToks,
      [](const RefoldLexBoundaryToken &l, const RefoldLexBoundaryToken &r) {
        return l.spelling == r.spelling;
      });
}

} // namespace

SmallVector<uint32_t, 4> RefoldMacroFormalDependenceProof::RewrittenFormals(
    StringRef invocationText, ArrayRef<std::pair<size_t, size_t>> argRanges,
    const DenseMap<uint32_t, std::string> &replByArgIdx,
    const clang::LangOptions &lexLang) {
  SmallVector<uint32_t, 4> formals;
  for (const auto &[argIdx, replacement] : replByArgIdx) {
    StringRef current;
    if (argIdx < argRanges.size()) {
      const auto range = argRanges[argIdx];
      if (range.first <= range.second && range.second <= invocationText.size())
        current = invocationText.slice(range.first, range.second);
    }
    if (!tokenEquivalent(current, replacement, lexLang))
      formals.push_back(argIdx);
  }
  llvm::sort(formals);
  return formals;
}

SmallVector<uint64_t, 32>
RefoldMacroFormalDependenceProof::CoverTokensOutsideHunks(
    const RefoldModel::MacroInvocation &root, ArrayRef<diffutils::Hunk> hunks) {
  SmallVector<uint64_t, 32> tokens;
  const std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(root);
  if (!cover)
    return tokens;
  for (uint64_t tok = cover->first; tok < cover->second; ++tok)
    if (llvm::none_of(hunks, [tok](const diffutils::Hunk &hunk) {
          return hunk.aStart <= tok && tok < hunk.aEnd;
        }))
      tokens.push_back(tok);
  return tokens;
}

bool RefoldMacroFormalDependenceProof::IsCallerDescendant(
    const RefoldModel::MacroInvocation &inv, uint64_t ancestorId) const {
  const RefoldModel::MacroInvocation *cur = &inv;
  for (size_t hops = 0, limit = deps_.model.GetMacroInvocations().size();
       hops <= limit && cur->callerMacroId; ++hops) {
    if (*cur->callerMacroId == ancestorId)
      return true;
    cur = deps_.macroTopology.FindMacroInvocationById(*cur->callerMacroId);
    if (!cur)
      return false;
  }
  return false;
}

RefoldMacroFormalDependenceProof::CoverRole
RefoldMacroFormalDependenceProof::ClassifyCoverInvocation(
    const RefoldModel::MacroInvocation &root, ArrayRef<uint64_t> rootAncestors,
    const RefoldModel::MacroInvocation &inv) const {
  if (llvm::is_contained(rootAncestors, inv.id) ||
      callsiteInside(inv, root, inv.invB, inv.invE))
    return CoverRole::Enclosing;
  for (const RefoldModel::MacroInvocation::OptByteRange &actual :
       root.invArgRanges)
    if (callsiteInside(root, inv, actual.first, actual.second))
      return CoverRole::ActualMaterial;
  if (IsCallerDescendant(inv, root.id))
    return CoverRole::Descendant;
  return CoverRole::Unattributed;
}

void RefoldMacroFormalDependenceProof::AddFormalRootDependence(
    const RefoldModel::MacroInvocation &root,
    const RefoldModel::MacroInvocation &inv, uint32_t formal,
    Dependence &dependence) const {
  SmallVector<std::pair<const RefoldModel::MacroInvocation *, uint32_t>, 8>
      worklist = {{&inv, formal}};
  DenseSet<std::pair<uint64_t, uint32_t>> visited;
  while (!worklist.empty()) {
    const auto [cur, curFormal] = worklist.pop_back_val();
    if (!visited.insert({cur->id, curFormal}).second)
      continue;
    std::optional<SmallVector<uint32_t, 4>> callerFormals =
        callerFormalsOfFormal(*cur, curFormal);
    if (!callerFormals || !cur->callerMacroId) {
      dependence.known = false;
      return;
    }
    if (*cur->callerMacroId == root.id) {
      append_range(dependence.formals, *callerFormals);
      continue;
    }
    const RefoldModel::MacroInvocation *caller =
        deps_.macroTopology.FindMacroInvocationById(*cur->callerMacroId);
    if (!caller) {
      dependence.known = false;
      return;
    }
    for (uint32_t callerFormal : *callerFormals)
      worklist.push_back({caller, callerFormal});
  }
}

void RefoldMacroFormalDependenceProof::AddCalleeRootDependence(
    const RefoldModel::MacroInvocation &root,
    const RefoldModel::MacroInvocation &inv,
    bool opaqueSelectorAttributedByRoot, Dependence &dependence) const {
  const RefoldModel::MacroCalleeOrigin &origin = inv.calleeOrigin;
  switch (origin.kind) {
  case MacroCalleeOriginKind::LiteralMacroName:
    return;
  case MacroCalleeOriginKind::CallerParam: {
    if (origin.callerParamIndices.empty() || !inv.callerMacroId) {
      dependence.known = false;
      return;
    }
    if (*inv.callerMacroId == root.id) {
      append_range(dependence.formals, origin.callerParamIndices);
      return;
    }
    const RefoldModel::MacroInvocation *caller =
        deps_.macroTopology.FindMacroInvocationById(*inv.callerMacroId);
    if (!caller) {
      dependence.known = false;
      return;
    }
    for (uint32_t callerFormal : origin.callerParamIndices)
      AddFormalRootDependence(root, *caller, callerFormal, dependence);
    return;
  }
  case MacroCalleeOriginKind::Paste:
    if (origin.parts.empty()) {
      dependence.known = false;
      return;
    }
    for (const RefoldModel::CalleeOriginPart &part : origin.parts) {
      if (part.kind == RefoldModel::CalleeOriginPartKind::Literal)
        continue;
      if (!part.rootMacroId || *part.rootMacroId != root.id ||
          !part.rootParamIndex) {
        dependence.known = false;
        return;
      }
      dependence.formals.push_back(*part.rootParamIndex);
    }
    return;
  case MacroCalleeOriginKind::Opaque:
    // A resolved opaque edge adds nothing here: the attributed root formals
    // are either already in `dependence` or replayed by the caller.
    if (!opaqueSelectorAttributedByRoot)
      dependence.known = false;
    return;
  }
}

bool RefoldMacroFormalDependenceProof::TokensIgnoreRewrittenFormals(
    const RefoldModel::MacroInvocation &root, ArrayRef<uint64_t> tokens,
    ArrayRef<uint32_t> rewrittenFormals,
    RootOccurrences rootOccurrences) const {
  if (rewrittenFormals.empty() || tokens.empty())
    return true;
  const std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(root);
  if (!cover)
    return false;

  SmallVector<uint64_t, 4> rootAncestors;
  for (const RefoldModel::MacroInvocation *cur = &root;
       cur->callerMacroId &&
       rootAncestors.size() <= deps_.model.GetMacroInvocations().size();) {
    rootAncestors.push_back(*cur->callerMacroId);
    cur = deps_.macroTopology.FindMacroInvocationById(*cur->callerMacroId);
    if (!cur)
      break;
  }

  // Every other invocation that produced a token of the cover, with its role.
  // A paste anywhere in the root's expansion can spell a callee name from
  // several actuals, which the opaque-edge resolution cannot see.
  SmallVector<std::pair<const RefoldModel::MacroInvocation *, CoverRole>, 16>
      producers;
  bool expansionPastes = !root.pasteTokens.empty() || !root.pasteSpans.empty();
  for (const RefoldModel::MacroInvocation &inv :
       deps_.model.GetMacroInvocations()) {
    if (inv.id == root.id ||
        llvm::none_of(inv.spans, [&](const RefoldModel::PPSpan &span) {
          return span.begin < cover->second && cover->first < span.end;
        }))
      continue;
    const CoverRole role = ClassifyCoverInvocation(root, rootAncestors, inv);
    if (role == CoverRole::Descendant &&
        (!inv.pasteTokens.empty() || !inv.pasteSpans.empty()))
      expansionPastes = true;
    producers.push_back({&inv, role});
  }

  for (uint64_t tok : tokens) {
    if (tok < cover->first || tok >= cover->second)
      return false;

    Dependence dependence;
    SmallVector<uint32_t, 4> rootFormals;
    appendFormalsObservedAtToken(root, tok, rootFormals);
    const bool rootAttributed = !rootFormals.empty();
    if (rootOccurrences == RootOccurrences::Observed)
      append_range(dependence.formals, rootFormals);

    SmallVector<const RefoldModel::MacroInvocation *, 8> descendants;
    for (const auto &[inv, role] : producers) {
      // Material spelled inside a root actual reaches the cover only through
      // the root's argument occurrences, so judge it by the tokens it claims;
      // its `spans` envelope can reach over the root's own replacement list.
      if (role == CoverRole::ActualMaterial
              ? !invocationClaimsToken(*inv, tok)
              : !spansContainToken(inv->spans, tok))
        continue;
      switch (role) {
      case CoverRole::Enclosing:
        break;
      case CoverRole::ActualMaterial:
        dependence.known &= rootAttributed;
        break;
      case CoverRole::Descendant:
        descendants.push_back(inv);
        break;
      case CoverRole::Unattributed:
        dependence.known = false;
        break;
      }
    }

    for (const RefoldModel::MacroInvocation *inv : descendants) {
      SmallVector<uint32_t, 4> observed;
      appendFormalsObservedAtToken(*inv, tok, observed);
      for (uint32_t formal : observed)
        AddFormalRootDependence(root, *inv, formal, dependence);
      const bool ownReplacementListToken =
          observed.empty() &&
          llvm::none_of(
              descendants, [&](const RefoldModel::MacroInvocation *other) {
                return other != inv && IsCallerDescendant(*other, inv->id);
              });
      AddCalleeRootDependence(root, *inv,
                              ownReplacementListToken && rootAttributed &&
                                  !expansionPastes,
                              dependence);
      // The invocations between `inv` and the root selected it too.
      for (const RefoldModel::MacroInvocation *cur = inv;
           dependence.known && *cur->callerMacroId != root.id;) {
        cur = deps_.macroTopology.FindMacroInvocationById(*cur->callerMacroId);
        if (!cur || !cur->callerMacroId) {
          dependence.known = false;
          break;
        }
        AddCalleeRootDependence(root, *cur,
                                /*opaqueSelectorAttributedByRoot=*/false,
                                dependence);
      }
    }

    const bool observesRewrite =
        llvm::any_of(dependence.formals, [&](uint32_t formal) {
          return llvm::is_contained(rewrittenFormals, formal);
        });
    if (!dependence.known || observesRewrite) {
      REFOLD_LOG_TRACE("macro/formal-dependence",
                       "reject inv id={0} name={1}: unchanged A token {2} {3}",
                       root.id, root.name, tok,
                       dependence.known ? "observes a rewritten root formal"
                                        : "has no producer provenance");
      return false;
    }
  }
  return true;
}

} // namespace refold
} // namespace clang
