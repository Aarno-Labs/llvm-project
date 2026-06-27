//===--- RefoldProofLattice.h ----------------------------------*- C++ -*-===//
//
// Accepted-result proof lattice boundary for clang-refold.
//
// Phase 4 keeps the theorem carrier types and member declarations in
// RefoldEngine.h because many of those value types still mention engine-local
// owner, state, and macro proof records.  The implementation lives in
// RefoldProofLattice.cpp so proof-summary normalization, accepted-result
// ranking, and selector logic are physically separated from the orchestration
// code in RefoldEngine.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H

#include "RefoldLog.h"
#include "RefoldEngine.h"

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H
