//===- PPMacroPrinting.h - Frontend-internal macro printing -----*- C++ -*-===//
//
// This file declares a small helper for printing macro definitions in a form
// accepted by the preprocessor (#define …) — shared by PPO and refold map.
//
// This header is private to clang/lib/Frontend/ (not installed).
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_FRONTEND_PPMACROPRINTING_H
#define LLVM_CLANG_FRONTEND_PPMACROPRINTING_H

#include "clang/Basic/LLVM.h"
#include "clang/Lex/Preprocessor.h"

namespace clang {

/// PrintMacroDefinition - Print a macro definition in a form that will be
/// properly accepted back as a definition.
void PrintMacroDefinition(const IdentifierInfo &II, const MacroInfo &MI,
                          Preprocessor &PP, raw_ostream *OS);

} // namespace clang

#endif // LLVM_CLANG_FRONTEND_PPMACROPRINTING_H
