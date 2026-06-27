//===--- RefoldIncludePathProof.h ------------------------------*- C++ -*-===//
//
// Shared syntactic include-operand path proof helpers for clang-refold.
//
// These helpers prove only the spelling-level safety of include operands the
// refolder may synthesize or inspect.  They do not prove include replay
// identity, include search-chain equivalence, physical file identity, or
// location-observer stability; callers must perform those proof obligations in
// their own replay/source-graph context.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEPATHPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEPATHPROOF_H

#include "llvm/ADT/StringRef.h"

namespace clang {
namespace refold {

/// Return true when the slash-separated operand contains a parent-directory
/// component.  This is a syntactic check over the operand spelling, not a
/// filesystem normalization or canonicalization step.
bool includeOperandHasParentComponent(llvm::StringRef Path);

/// Return true when \p Path is a relative include operand spelling that may be
/// synthesized into emitted source.  The accepted grammar is intentionally
/// narrow: non-empty ASCII path components using [A-Za-z0-9_./-], with no
/// backslashes, quotes, absolute path spelling, empty components, `.`, or `..`.
///
/// This does not prove that replaying the include will select the producer file
/// or preserve filename observers; it only proves the no-escape syntactic
/// operand contract shared by include replay and source-graph planning.
bool safeSynthesizedRelativeIncludeOperandPath(llvm::StringRef Path);

/// Predicate for synthesized relative operands that may be emitted into an
/// include directive after the caller has separately proven replay/source-graph
/// semantics.  The caller chooses quoted vs. angled delimiters; this helper only
/// enforces the relative no-escape path contract.
bool safeSynthesizedRelativeIncludeOperand(llvm::StringRef Path);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEPATHPROOF_H
