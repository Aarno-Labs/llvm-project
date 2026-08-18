// RUN: %clang-refold-tester-relaxed-expect-refold-fail pragma_once_operator_midline_conditional_refuses_hoist
// RUN: FileCheck %s --check-prefix=AUDIT < %t/outputs/pragma_once_operator_midline_conditional_refuses_hoist.out
// Fail-closed regression: a conditional `_Pragma("once")` cannot be guarded from
// the top of the body.
//
// This is the negative half of pragma_once_operator_midline_guards_at_include_site.c,
// and the two differ only in that this operator sits inside a conditional arm.
//
// An operator-spelled once site is never rewritten in place: the emission
// firewall does not admit `PragmaOperator` to the guard-rewrite authority, so
// source materialization cannot write a `#define` over the operator's bytes
// whether or not it owns its line.  The only path that can guard such a header
// is the B-realized one, and a B realization contains tokens rather than header
// source -- the `#if`/`#endif` are gone -- so the define has nowhere to go but
// the top of the body.
//
// Top-of-body placement fires the define on entry, which matches the original
// only when the pragma fires on entry too.  Here it fires only when the arm is
// taken, so a configuration that does not take the arm would find the header
// already "once"-d and skip a body the original would have re-emitted.  The
// dominance proof fails and the refold is refused.
//
// The arm is `#if 1`, always taken -- and that is the point.  The rewrite does
// not evaluate the condition to discover it; it refuses on arm membership alone,
// because deciding otherwise means constant-folding preprocessor conditions to
// justify moving preprocessor state.
//
// PINS A REFUSAL, NOT AN OUTPUT.  Making this fold needs a third placement that
// neither path has: keeping the define at the site by opening a line for it, so
// it stays inside the arm.  That requires admitting `PragmaOperator` to the
// guard-rewrite authority together with the line-ownership check named in the
// comment there, and it costs a physical line of drift at the site.
//
// AUDIT: realized from B needs a top-of-body define
// AUDIT: no admissible refold
#include "guard_once_operator_midline_conditional.h"

int mid = 0;

#include "guard_once_operator_midline_conditional.h"

int tail = MIDLINE_COND_V;
