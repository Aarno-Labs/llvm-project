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
// The materialized body is guarded exactly as a `#pragma once` body is: the
// `#define` replaces the operator's own bytes, in the copy of the header being
// spliced into the TU, so the header's own `#define GUARD_OP_V 4` survives as
// source rather than being realized from tokens.  The second include is
// preserved under the same guard instead of being dropped.
//
// This operator owns its physical line, so the replacement needs no line of its
// own.  pragma_once_operator_midline_guards_at_include_site.c covers the
// operator that shares a line, and
// pragma_once_operator_midline_conditional_guards_inside_arm.c the one that
// also sits in a conditional arm.
#include "guard_once_operator.h"

int mid = 0;

#include "guard_once_operator.h"

int tail = GUARD_OP_V;
