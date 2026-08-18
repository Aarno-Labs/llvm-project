// RUN: %clang-refold-tester pragma_once_operator_guards_materialized_body
// Regression: `_Pragma("once")` establishes the same once-state as
// `#pragma once` and is now admitted on the same terms.
//
// Clang consumes a `once` pragma inside its handler, so an operator-spelled one
// never reaches the pragma-printing path that reconstructs a `#pragma` spelling
// for passed-through pragmas.  The producer therefore recorded nothing for it,
// the header's once-state was invisible to the guard catalog, and the catalog
// correctly failed closed -- surrendering the whole translation unit to the raw
// edited preprocessed stream.  The producer now records the operator event
// itself, so the fact exists and the catalog can use it.
//
// Admission is gated on the interval carrying an exact producer binding.  That
// binding is what proves the operator executed in this include-owner domain;
// the spelling alone proves nothing, because the same bytes could sit in a
// macro replacement list or in a conditional arm that was never taken.
//
// The materialized body is guarded exactly as a `#pragma once` body is, and the
// second include is preserved under the same guard instead of being dropped.
#define GUARD_OP_V 4
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
int from_guard_op = 9;

#endif

int mid = 0;

#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "guard_once_operator.h"
#endif

int tail = GUARD_OP_V;
