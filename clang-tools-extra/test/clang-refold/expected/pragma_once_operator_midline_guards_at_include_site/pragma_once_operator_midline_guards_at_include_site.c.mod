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
// That refusal was in the wrong place.  It is true of one rewrite -- replacing
// the operator's own bytes -- but the catalog decides whether the header's
// once-state is usable at all, so refusing there discarded it for every path.
// An operator-spelled site is never rewritten in place anyway: the emission
// firewall does not admit `PragmaOperator` to the guard-rewrite authority.  The
// path that actually guards such a header is the B-realized one, which wraps the
// body in `#ifndef`/`#define` at its top and proves what that placement needs --
// the pragma fires on entry to the body, and the header does not include itself,
// a self-include being the only construct that can observe once-state between
// the body's first byte and the site.  None of that cares whether the operator
// owns its line.
//
// So the fix is a deletion: the line-ownership test is gone, and the pre-existing
// proof does the work.  Note what that preserves -- `int from_midline_op` keeps
// its place on the line the operator shared, the header on disk is untouched,
// and the second include survives under the same guard instead of being dropped.
//
// pragma_once_operator_midline_conditional_refuses_hoist.c pins the case this
// does not reach: a conditional operator, where top-of-body placement is not
// equivalent and no other placement is available.
#define MIDLINE_OP_V 4
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
int from_midline_op = 9;
int mid_op_use = 4;

#endif

int mid = 0;

#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "guard_once_operator_midline.h"
#endif

int tail = MIDLINE_OP_V;
