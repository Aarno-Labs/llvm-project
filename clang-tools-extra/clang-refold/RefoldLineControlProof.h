//===--- RefoldLineControlProof.h -----------------------------*- C++ -*-===//
//
// Final line-control proof helpers for clang-refold.
//
// This module is the home for proof-only line-state queries and final
// line-control audit gates that were previously buried in RefoldEngine's tail
// utility fragment.  It deliberately does not own text emission: RefoldEngine
// still constructs replacements, inserts directives, streams original source,
// and runs the final pruner.  The helpers implemented by
// RefoldLineControlProof.cpp only answer proof/audit questions used by those
// emission sites.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLPROOF_H

#include "FinalLineControlModel.h"
#include "RefoldProofTypes.h"

namespace clang {
namespace refold {

class RefoldEngine;

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLPROOF_H
