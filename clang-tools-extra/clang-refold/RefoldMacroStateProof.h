//===--- RefoldMacroStateProof.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Macro-state proof helpers.
//
// This module owns the shared proof primitives for #define/#undef state motion
// and the materialized-header macro-state stabilization planner.  RefoldEngine
// still owns actual TextEdit construction, accepted-result attachment, terminal
// fallback request state, and include/TU orchestration.
//
// The API is currently expressed as private RefoldEngine member declarations in
// RefoldEngine.h.  That keeps this follow-up extraction mechanical: it moves
// definitions into the macro-state proof translation unit without changing call
// sites, proof policy, or emitted source.  A later dependency-narrowing patch
// can introduce a standalone MacroStateProofContext once the remaining TU-level
// macro-state carry pass is separated from edit mutation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTATEPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTATEPROOF_H

namespace clang {
namespace refold {

// See RefoldEngine.h for the private member declarations implemented by
// RefoldMacroStateProof.cpp.

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTATEPROOF_H
