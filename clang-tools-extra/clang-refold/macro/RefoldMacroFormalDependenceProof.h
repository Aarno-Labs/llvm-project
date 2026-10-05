//===--- RefoldMacroFormalDependenceProof.h ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Producer-backed proof that tokens of a root invocation's cover do not depend
// on the root formals an args-only rewrite changes.
//
// An args-only rewrite edits root actuals and lets the preprocessor re-expand
// everything that observes them.  Each admission path proves the occurrences
// it solved, but a token B left unchanged is admissible only when nothing that
// produced it observes a rewritten formal.  The proof composes the producer's
// edge-local facts -- root occurrence spans, descendant `arg_deps`, and callee
// origins -- and fails closed wherever one of them is missing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROFORMALDEPENDENCEPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROFORMALDEPENDENCEPROOF_H

#include "model/RefoldModel.h"
#include "source/RefoldDiffTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace clang {

class LangOptions;

namespace refold {

class RefoldMacroTopology;

/// Proves that A tokens of a root invocation's cover are independent of a set
/// of rewritten root formals.
class RefoldMacroFormalDependenceProof {
public:
  /// Borrowed producer model and caller-graph index; both must outlive the
  /// proof object.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldMacroTopology &macroTopology;
  };

  /// Whether the root's own occurrence spans count as dependence.
  enum class RootOccurrences {
    /// The caller solved only some occurrences, so a root occurrence of a
    /// rewritten formal is itself a dependent token.
    Observed,
    /// The caller already replayed every root occurrence of each rewritten
    /// formal against B, including partial edits of a multi-token actual.
    Validated,
  };

  explicit RefoldMacroFormalDependenceProof(Dependencies deps) : deps_(deps) {}

  /// Return whether no A token in \p tokens, each inside \p root's cover,
  /// depends on a formal in \p rewrittenFormals.
  ///
  /// A token depends on the root formals its producer attributions name: root
  /// occurrence spans (per \p rootOccurrences), the composed `arg_deps` of
  /// every descendant occurrence that produced it, and the callee origin of
  /// every descendant on its caller chain.  An attribution the producer did
  /// not record makes the token depend on every formal.
  bool TokensIgnoreRewrittenFormals(const RefoldModel::MacroInvocation &root,
                                    llvm::ArrayRef<uint64_t> tokens,
                                    llvm::ArrayRef<uint32_t> rewrittenFormals,
                                    RootOccurrences rootOccurrences) const {
    return TokensAvoidFormals(root, tokens, rewrittenFormals, rootOccurrences,
                              /*countArgumentOccurrences=*/true);
  }

  /// Return whether no invocation that produced an A token of `[begin, end)`
  /// inside \p root's cover was selected, through its callee or an ancestor's,
  /// by a formal in \p formals.
  ///
  /// Root occurrence spans that copy an actual and those that hold a callee
  /// the actual selected look alike; only the second is answered here.
  /// Unknown selection provenance counts as selected.
  bool SelectionIgnoresFormals(const RefoldModel::MacroInvocation &root,
                               uint64_t begin, uint64_t end,
                               llvm::ArrayRef<uint32_t> formals) const;

  /// Return whether one of \p root's own occurrence spans of a formal in
  /// \p formals produced an A token in \p tokens.
  static bool RootOccurrencesObserve(const RefoldModel::MacroInvocation &root,
                                     llvm::ArrayRef<uint64_t> tokens,
                                     llvm::ArrayRef<uint32_t> formals);

  /// Return, ascending, the formals whose replacement in \p replByArgIdx is not
  /// token-equivalent to the actual spelled at \p argRanges in
  /// \p invocationText.
  static llvm::SmallVector<uint32_t, 4>
  RewrittenFormals(llvm::StringRef invocationText,
                   llvm::ArrayRef<std::pair<size_t, size_t>> argRanges,
                   const llvm::DenseMap<uint32_t, std::string> &replByArgIdx,
                   const clang::LangOptions &lexLang);

  /// Return the A tokens of \p root's whole cover that no hunk in \p hunks
  /// replaces or deletes.
  static llvm::SmallVector<uint64_t, 32>
  CoverTokensOutsideHunks(const RefoldModel::MacroInvocation &root,
                          llvm::ArrayRef<diffutils::Hunk> hunks);

private:
  /// Shared body of `TokensIgnoreRewrittenFormals` and
  /// `SelectionIgnoresFormals`; the latter does not count descendants'
  /// argument occurrences.
  bool TokensAvoidFormals(const RefoldModel::MacroInvocation &root,
                          llvm::ArrayRef<uint64_t> tokens,
                          llvm::ArrayRef<uint32_t> formals,
                          RootOccurrences rootOccurrences,
                          bool countArgumentOccurrences) const;

  /// Root formals that the producer's attributions of one A token depend on.
  struct Dependence {
    llvm::SmallVector<uint32_t, 4> formals;
    /// False once an attribution lacks the provenance needed to name its root
    /// formals.
    bool known = true;
  };

  /// Role of a non-root invocation that produced tokens of the root's cover.
  enum class CoverRole {
    /// Selected without the root's expansion: a caller-chain ancestor, a
    /// callsite whose spelling contains the root's, or a literal callee whose
    /// callers are literal up to a root ancestor or the translation unit.  Its
    /// view of the cover is the root's own output, passed through.
    Enclosing,
    /// Spelled inside one of the root's actuals, so its output reaches the
    /// cover only through the root's own argument occurrences.
    ActualMaterial,
    /// Generated by the root's expansion through recorded caller edges.
    Descendant,
    /// None of the above is proven.
    Unattributed,
  };

  /// Classify \p inv, which produced tokens of \p root's cover.
  /// \p rootAncestors are the invocations on \p root's own caller chain.
  CoverRole
  ClassifyCoverInvocation(const RefoldModel::MacroInvocation &root,
                          llvm::ArrayRef<uint64_t> rootAncestors,
                          const RefoldModel::MacroInvocation &inv) const;

  /// Return whether \p inv and every caller up to a member of
  /// \p rootAncestors, or up to the translation unit, have a literal callee,
  /// so that nothing the root expands selects \p inv.
  bool SelectedOutsideRoot(const RefoldModel::MacroInvocation &inv,
                           llvm::ArrayRef<uint64_t> rootAncestors) const;

  /// Return whether recorded `caller_macro_id` edges lead from \p inv to the
  /// invocation \p ancestorId.
  bool IsCallerDescendant(const RefoldModel::MacroInvocation &inv,
                          uint64_t ancestorId) const;

  /// Add the root formals that formal \p formal of the descendant \p inv is
  /// spelled from, composing `arg_deps` edge by edge up to \p root.
  void AddFormalRootDependence(const RefoldModel::MacroInvocation &root,
                               const RefoldModel::MacroInvocation &inv,
                               uint32_t formal, Dependence &dependence) const;

  /// Add the root formals that select the callee of the descendant \p inv at
  /// its recorded caller edge.
  ///
  /// A literal callee is replacement-list text.  A `caller_param` callee
  /// composes through \p inv's caller, and a pasted callee names its root
  /// argument slices directly.  An opaque callee is only known to come from
  /// some macro argument.  It is resolved solely by
  /// \p opaqueSelectorRootFormals, set only when the token being proven is
  /// \p inv's own replacement-list text, lies in a root argument occurrence,
  /// and no token was pasted anywhere in the root's expansion.  The producer
  /// attributes a token to a root argument by its file location, which for
  /// \p inv's own replacement-list text is the file location of \p inv's
  /// callee name.  Without a paste that name is a verbatim token of the
  /// attributed actual, so the attributing formals select \p inv.  Every other
  /// opaque edge makes \p dependence unknown.
  void AddCalleeRootDependence(
      const RefoldModel::MacroInvocation &root,
      const RefoldModel::MacroInvocation &inv,
      const llvm::SmallVectorImpl<uint32_t> *opaqueSelectorRootFormals,
      Dependence &dependence) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROFORMALDEPENDENCEPROOF_H
