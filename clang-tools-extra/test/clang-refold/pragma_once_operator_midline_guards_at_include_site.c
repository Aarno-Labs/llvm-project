// RUN: %clang-refold-tester pragma_once_operator_midline_guards_at_include_site
// Regression: once-state spelled by a mid-line `_Pragma` is guarded rather than
// surrendering the translation unit.
//
// This is the companion of pragma_once_operator_guards_materialized_body.c,
// differing only in that the operator shares its line with a declaration.
// `_Pragma("once")` in that position is a legal expression, and the catalog used
// to refuse the header outright on the grounds that a `#define` written over
// those bytes would not begin a logical line.
//
// That refusal was in the wrong place.  It is true of one *spelling* of the
// rewrite -- writing `#define` over the operator's bytes and nothing else --
// but the catalog decides whether the header's once-state is usable at all, so
// refusing there discarded it for every path.
//
// Line ownership is decided at the site instead, where it is a question about
// one edit rather than about the header.  An operator that shares its line has
// a physical line opened for the directive, which is what the replacement below
// does: `int from_midline_op` keeps its place on the line it had, and the
// `#define` follows on a line of its own.  That costs one line of drift in the
// emitted body, admissible under the same suffix line-observer proof the
// `#ifndef` prologue already needs, because the prologue shifts the whole body
// and a site shifts only the suffix after it.
//
// Note what the in-place rewrite preserves that a body realized from B does
// not: the header's own `#define MIDLINE_OP_V 4` and `int mid_op_use` survive
// as source.  The header on disk is untouched -- every edit lands in the copy
// spliced into the TU -- and the second include survives under the same guard
// instead of being dropped.
//
// pragma_once_operator_midline_conditional_guards_inside_arm.c carries the same
// operator inside a conditional arm, where the define must stay in the arm.
#include "guard_once_operator_midline.h"

int mid = 0;

#include "guard_once_operator_midline.h"

int tail = MIDLINE_OP_V;
