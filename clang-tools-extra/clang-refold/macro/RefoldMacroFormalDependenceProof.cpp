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

/// Return the A token at or after which \p inv's output begins, when the
/// producer credited \p inv with no token.
///
/// The producer opens an invocation's `spans` with an empty span at the
/// printed-token index of its expansion callback, which precedes the printing
/// of any of its output, and widens it only over tokens it attributes to that
/// invocation.  An invocation that only passes its arguments through, such as
/// `F(v) v` selected by a caller's formal, can keep the empty span while its
/// whole output is credited to other invocations.  Its output is then some
/// unknown run of tokens at or after the anchor, possibly none.
std::optional<uint64_t>
uncreditedOutputAnchor(const RefoldModel::MacroInvocation &inv) {
  if (inv.spans.empty() ||
      llvm::any_of(inv.spans, [](const RefoldModel::PPSpan &span) {
        return span.begin < span.end;
      }))
    return std::nullopt;
  uint64_t anchor = inv.spans.front().begin;
  for (const RefoldModel::PPSpan &span : inv.spans)
    anchor = std::min(anchor, span.begin);
  return anchor;
}

/// Return whether \p inv may have produced a token of the half-open A-token
/// range \p cover.
///
/// One credited with no token counts when its output could begin inside the
/// cover.  A descendant is expanded after the root, and once the producer
/// credits the root with a token the cover begins at the root's own anchor, so
/// a descendant is never anchored before the cover.
bool mayProduceCoverToken(const RefoldModel::MacroInvocation &inv,
                          std::pair<uint64_t, uint64_t> cover) {
  if (const std::optional<uint64_t> anchor = uncreditedOutputAnchor(inv))
    return cover.first <= *anchor && *anchor < cover.second;
  return llvm::any_of(inv.spans, [&](const RefoldModel::PPSpan &span) {
    return span.begin < cover.second && cover.first < span.end;
  });
}

/// The formals of one invocation that an actual of another depends on.
struct ActualSource {
  /// The invocation whose formals they are.
  uint64_t macroId = 0;
  SmallVector<uint32_t, 4> formals;
};

/// Return the formals that the actual bound to formal \p formal of \p inv
/// depends on, with the invocation they belong to.
///
/// Text `arg_deps` name the immediate caller's formals and are indexed by
/// actual, so a variadic formal collects every actual from its position on
/// and an omitted variadic actual references nothing.  Without them,
/// `arg_token_deps` name the formals of the invocation whose expansion held
/// the argument list, per formal.  Returns nullopt when neither is recorded,
/// or when text `arg_deps` lack an entry for a non-variadic formal.
std::optional<ActualSource>
actualSourceOfFormal(const RefoldModel::MacroInvocation &inv, uint32_t formal) {
  if (inv.argDeps.empty()) {
    if (!inv.argTokenDeps || formal >= inv.argTokenDeps->formals.size())
      return std::nullopt;
    ActualSource source{inv.argTokenDeps->macroId, {}};
    append_range(source.formals, inv.argTokenDeps->formals[formal]);
    return source;
  }
  if (!inv.callerMacroId)
    return std::nullopt;
  const bool variadic =
      formal < inv.defParams.size() && inv.defParams[formal].variadic;
  if (!variadic && formal >= inv.argDeps.size())
    return std::nullopt;
  ActualSource source{*inv.callerMacroId, {}};
  const size_t end = variadic ? inv.argDeps.size() : size_t(formal) + 1;
  for (size_t actual = formal; actual < end; ++actual)
    append_range(source.formals, inv.argDeps[actual]);
  return source;
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

/// Return whether \p dependence names a formal in \p formals.
bool namesAnyFormal(ArrayRef<uint32_t> dependence, ArrayRef<uint32_t> formals) {
  return llvm::any_of(dependence, [&](uint32_t formal) {
    return llvm::is_contained(formals, formal);
  });
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
  // A call the root's expansion generated was formed after the root's callsite
  // was consumed, so that callsite is never inside its arguments.  Its
  // `[inv_b, inv_e)` can still contain the callsite: the envelope runs from
  // where its name was spelled, perhaps the root's replacement list, to its
  // `)`, perhaps spelled in source after the root's callsite.
  if (llvm::is_contained(rootAncestors, inv.id) ||
      (callsiteInside(inv, root, inv.invB, inv.invE) &&
       !IsCallerDescendant(inv, root.id)))
    return CoverRole::Enclosing;
  for (const RefoldModel::MacroInvocation::OptByteRange &actual :
       root.invArgRanges)
    if (callsiteInside(root, inv, actual.first, actual.second))
      return CoverRole::ActualMaterial;
  if (IsCallerDescendant(inv, root.id))
    return CoverRole::Descendant;
  if (SelectedOutsideRoot(inv, rootAncestors))
    return CoverRole::Enclosing;
  return CoverRole::Unattributed;
}

bool RefoldMacroFormalDependenceProof::SelectedOutsideRoot(
    const RefoldModel::MacroInvocation &inv,
    ArrayRef<uint64_t> rootAncestors) const {
  const RefoldModel::MacroInvocation *cur = &inv;
  for (size_t hops = 0, limit = deps_.model.GetMacroInvocations().size();
       hops <= limit; ++hops) {
    if (cur->calleeOrigin.kind != MacroCalleeOriginKind::LiteralMacroName)
      return false;
    if (!cur->callerMacroId ||
        llvm::is_contained(rootAncestors, *cur->callerMacroId))
      return true;
    cur = deps_.macroTopology.FindMacroInvocationById(*cur->callerMacroId);
    if (!cur)
      return false;
  }
  return false;
}

const RefoldModel::MacroInvocation *
RefoldMacroFormalDependenceProof::AncestorBelowRoot(
    const RefoldModel::MacroInvocation &root,
    const RefoldModel::MacroInvocation &inv, uint64_t ancestorId) const {
  if (ancestorId == root.id || !IsCallerDescendant(inv, ancestorId))
    return nullptr;
  const RefoldModel::MacroInvocation *ancestor =
      deps_.macroTopology.FindMacroInvocationById(ancestorId);
  if (!ancestor || !IsCallerDescendant(*ancestor, root.id))
    return nullptr;
  return ancestor;
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
    std::optional<ActualSource> source = actualSourceOfFormal(*cur, curFormal);
    if (!source) {
      dependence.known = false;
      return;
    }
    if (source->macroId == root.id) {
      append_range(dependence.formals, source->formals);
      continue;
    }
    // Composing further keeps to `inv`'s caller chain, whose selections the
    // callers of this proof add.
    const RefoldModel::MacroInvocation *holder =
        AncestorBelowRoot(root, *cur, source->macroId);
    if (!holder) {
      dependence.known = false;
      return;
    }
    for (uint32_t holderFormal : source->formals)
      worklist.push_back({holder, holderFormal});
  }
}

std::optional<SmallVector<uint32_t, 1>>
RefoldMacroFormalDependenceProof::CalleeSubstRootFormals(
    const RefoldModel::MacroInvocation &root,
    const RefoldModel::MacroInvocation &inv) const {
  for (const RefoldModel::MacroTokenOrigin &hop : inv.calleeSubst) {
    if (hop.macroId == root.id) {
      SmallVector<uint32_t, 1> formals;
      if (hop.param)
        formals.push_back(*hop.param);
      return formals;
    }
    // A substitution or spelling inside the root's expansion, by an
    // invocation whose own selection the callers of this proof account for.
    if (!AncestorBelowRoot(root, inv, hop.macroId))
      return std::nullopt;
    if (!hop.param)
      return SmallVector<uint32_t, 1>();
  }
  return std::nullopt;
}

void RefoldMacroFormalDependenceProof::AddCalleeRootDependence(
    const RefoldModel::MacroInvocation &root,
    const RefoldModel::MacroInvocation &inv,
    const SmallVectorImpl<uint32_t> *opaqueSelectorRootFormals,
    Dependence &dependence) const {
  const RefoldModel::MacroCalleeOrigin &origin = inv.calleeOrigin;
  switch (origin.kind) {
  case MacroCalleeOriginKind::LiteralMacroName:
    return;
  case MacroCalleeOriginKind::CallerParam: {
    // Composing the caller's `arg_deps` over-approximates the selecting
    // formals through an actual that names several of them.
    if (std::optional<SmallVector<uint32_t, 1>> formals =
            CalleeSubstRootFormals(root, inv)) {
      append_range(dependence.formals, *formals);
      return;
    }
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
    if (opaqueSelectorRootFormals) {
      append_range(dependence.formals, *opaqueSelectorRootFormals);
      return;
    }
    if (std::optional<SmallVector<uint32_t, 1>> formals =
            CalleeSubstRootFormals(root, inv)) {
      append_range(dependence.formals, *formals);
      return;
    }
    dependence.known = false;
    return;
  }
}

bool RefoldMacroFormalDependenceProof::SelectionIgnoresFormals(
    const RefoldModel::MacroInvocation &root, uint64_t begin, uint64_t end,
    ArrayRef<uint32_t> formals) const {
  SmallVector<uint64_t, 8> tokens;
  for (uint64_t tok = begin; tok < end; ++tok)
    tokens.push_back(tok);
  return TokensAvoidFormals(root, tokens, formals, RootOccurrences::Validated,
                            /*countArgumentOccurrences=*/false);
}

std::optional<RefoldMacroFormalDependenceProof::CoverKeptTokens>
RefoldMacroFormalDependenceProof::CoverTokensKeptByRewrite(
    const RefoldModel::MacroInvocation &root,
    ArrayRef<uint32_t> rewrittenFormals) const {
  const std::optional<CoverAttributions> attributions =
      CollectCoverAttributions(root);
  if (!attributions)
    return std::nullopt;

  CoverKeptTokens result;
  result.cover = attributions->cover;
  for (uint64_t tok = result.cover.first; tok < result.cover.second; ++tok) {
    if (rewrittenFormals.empty()) {
      result.kept.push_back(true);
      continue;
    }
    const Dependence dependence =
        TokenDependence(root, *attributions, tok, RootOccurrences::Observed,
                        /*countArgumentOccurrences=*/true);
    result.kept.push_back(
        dependence.known &&
        !namesAnyFormal(dependence.formals, rewrittenFormals));
  }
  return result;
}

std::optional<RefoldMacroFormalDependenceProof::CoverAttributions>
RefoldMacroFormalDependenceProof::CollectCoverAttributions(
    const RefoldModel::MacroInvocation &root,
    std::optional<std::pair<uint64_t, uint64_t>> cover) const {
  if (!cover)
    cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(root);
  if (!cover)
    return std::nullopt;

  CoverAttributions attributions;
  attributions.cover = *cover;
  for (const RefoldModel::MacroInvocation *cur = &root;
       cur->callerMacroId && attributions.rootAncestors.size() <=
                                 deps_.model.GetMacroInvocations().size();) {
    attributions.rootAncestors.push_back(*cur->callerMacroId);
    cur = deps_.macroTopology.FindMacroInvocationById(*cur->callerMacroId);
    if (!cur)
      break;
  }

  // Every other invocation that produced a token of the cover, with its role.
  attributions.expansionPastes =
      !root.pasteTokens.empty() || !root.pasteSpans.empty();
  for (const RefoldModel::MacroInvocation &inv :
       deps_.model.GetMacroInvocations()) {
    if (inv.id == root.id || !mayProduceCoverToken(inv, *cover))
      continue;
    const CoverRole role =
        ClassifyCoverInvocation(root, attributions.rootAncestors, inv);
    if (role == CoverRole::Descendant &&
        (!inv.pasteTokens.empty() || !inv.pasteSpans.empty()))
      attributions.expansionPastes = true;
    attributions.producers.push_back({&inv, role});
  }
  return attributions;
}

RefoldMacroFormalDependenceProof::Dependence
RefoldMacroFormalDependenceProof::TokenDependence(
    const RefoldModel::MacroInvocation &root,
    const CoverAttributions &attributions, uint64_t tok,
    RootOccurrences rootOccurrences, bool countArgumentOccurrences) const {
  Dependence dependence;
  SmallVector<uint32_t, 4> rootFormals;
  appendFormalsObservedAtToken(root, tok, rootFormals);
  const bool rootAttributed = !rootFormals.empty();
  if (rootOccurrences == RootOccurrences::Observed)
    append_range(dependence.formals, rootFormals);

  // Each descendant that may have produced `tok`, and whether the producer
  // credited it with `tok`.
  SmallVector<std::pair<const RefoldModel::MacroInvocation *, bool>, 8>
      descendants;
  for (const auto &[inv, role] : attributions.producers) {
    // Material spelled inside a root actual reaches the cover only through
    // the root's argument occurrences, so judge it by the tokens it claims;
    // its `spans` envelope can reach over the root's own replacement list.
    const bool claimed = role == CoverRole::ActualMaterial
                             ? invocationClaimsToken(*inv, tok)
                             : spansContainToken(inv->spans, tok);
    if (!claimed) {
      // A descendant credited with nothing may still have produced `tok` by
      // passing it through from its arguments, as `F(v) v` does when a root
      // formal names `F`.  The token's own attributions carry what those
      // arguments observe, but not that the descendant was selected, so add
      // its selection, which is unknown where the producer recorded none.
      const std::optional<uint64_t> anchor = uncreditedOutputAnchor(*inv);
      if (role == CoverRole::Descendant && anchor && *anchor <= tok)
        descendants.push_back({inv, /*credited=*/false});
      continue;
    }
    switch (role) {
    case CoverRole::Enclosing:
      break;
    case CoverRole::ActualMaterial:
      dependence.known &= rootAttributed;
      break;
    case CoverRole::Descendant:
      descendants.push_back({inv, /*credited=*/true});
      break;
    case CoverRole::Unattributed:
      dependence.known = false;
      break;
    }
  }

  for (const auto &[inv, credited] : descendants) {
    SmallVector<uint32_t, 4> observed;
    appendFormalsObservedAtToken(*inv, tok, observed);
    if (countArgumentOccurrences)
      for (uint32_t formal : observed)
        AddFormalRootDependence(root, *inv, formal, dependence);
    const bool ownReplacementListToken =
        credited && observed.empty() &&
        llvm::none_of(descendants, [&](const auto &other) {
          return other.first != inv &&
                 IsCallerDescendant(*other.first, inv->id);
        });
    // A resolved opaque edge is selected by the attributing root formals.
    // `Observed` already counts them, and under `Validated` the caller
    // replayed those very occurrences; only the selection query adds them.
    const SmallVector<uint32_t, 4> noFormals;
    AddCalleeRootDependence(root, *inv,
                            !ownReplacementListToken || !rootAttributed ||
                                    attributions.expansionPastes
                                ? nullptr
                            : countArgumentOccurrences ? &noFormals
                                                       : &rootFormals,
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
                              /*opaqueSelectorRootFormals=*/nullptr,
                              dependence);
    }
  }
  return dependence;
}

bool RefoldMacroFormalDependenceProof::TokensAvoidFormals(
    const RefoldModel::MacroInvocation &root, ArrayRef<uint64_t> tokens,
    ArrayRef<uint32_t> rewrittenFormals, RootOccurrences rootOccurrences,
    bool countArgumentOccurrences,
    std::optional<std::pair<uint64_t, uint64_t>> cover) const {
  if (rewrittenFormals.empty() || tokens.empty())
    return true;
  const std::optional<CoverAttributions> attributions =
      CollectCoverAttributions(root, cover);
  if (!attributions)
    return false;

  for (uint64_t tok : tokens) {
    if (tok < attributions->cover.first || tok >= attributions->cover.second)
      return false;

    const Dependence dependence = TokenDependence(
        root, *attributions, tok, rootOccurrences, countArgumentOccurrences);
    const bool observesRewrite =
        namesAnyFormal(dependence.formals, rewrittenFormals);
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
