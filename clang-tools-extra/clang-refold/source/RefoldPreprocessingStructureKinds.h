//===--- RefoldPreprocessingStructureKinds.h -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Classification vocabulary for protected preprocessing structure, shared by
// the structure index that produces it and the edit carriers that record it.
// The `toString` definitions live with the index in
// RefoldPreprocessingStructureIndex.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSINGSTRUCTUREKINDS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSINGSTRUCTUREKINDS_H

#include "llvm/ADT/StringRef.h"

namespace clang {
namespace refold {

/// Physical preprocessing construct protected by the structure index.
///
/// Conditional-control directives are kept distinct because later structural
/// tiling must preserve their ordered group/arm topology.  `OtherDirective`
/// intentionally covers every lexically valid directive not modeled by a more
/// specific enumerator; unknown directives are protected rather than ignored.
enum class PreprocessingStructureKind {
  ConditionalIf,
  ConditionalIfdef,
  ConditionalIfndef,
  ConditionalElif,
  ConditionalElifdef,
  ConditionalElifndef,
  ConditionalElse,
  ConditionalEndif,
  MacroDefine,
  MacroUndef,
  Include,
  IncludeNext,
  Import,
  Pragma,
  PragmaOperator,
  LineControl,
  ErrorDirective,
  WarningDirective,
  OtherDirective,
};

/// Return a stable diagnostic spelling for a preprocessing-structure kind.
llvm::StringRef toString(PreprocessingStructureKind kind);

/// Producer record class bound to a lexical preprocessing interval.
///
/// Model ids live in several producer arrays and are not assumed to share one
/// namespace.  Carrying the record class prevents a line-control event id from
/// being mistaken for an item id with the same integer value.
enum class PreprocessingStructureModelKind {
  None,
  MacroDirective,
  IncludeDirective,
  PragmaDirective,
  LineControlEvent,
  ConditionalDirective,
};

/// Return a stable diagnostic spelling for a producer binding class.
llvm::StringRef toString(PreprocessingStructureModelKind kind);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPREPROCESSINGSTRUCTUREKINDS_H
