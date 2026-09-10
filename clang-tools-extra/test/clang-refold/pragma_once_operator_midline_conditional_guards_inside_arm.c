// RUN: %clang-refold-tester-relaxed pragma_once_operator_midline_conditional_guards_inside_arm
// Regression: a conditional `_Pragma("once")` is guarded at its own site, not
// from the top of the body.
//
// This is the third of the operator-spelled once shapes, after
// pragma_once_operator_guards_materialized_body.c (operator owns its line) and
// pragma_once_operator_midline_guards_at_include_site.c (operator shares one).
// Here it also sits inside a conditional arm, and that is what decides the
// placement.
//
// A body realized from B carries tokens rather than header source -- the
// `#if`/`#endif` are gone -- so its define has nowhere to go but the top of the
// body.  Top-of-body placement fires the define on entry, which matches the
// original only when the pragma fires on entry too.  Here it fires only when
// the arm is taken, so a configuration that did not take the arm would find the
// header already "once"-d and skip a body the original would have re-emitted.
// That placement is therefore refused, and correctly.
//
// The placement that works keeps the define where the pragma was.  Source
// materialization emits the header's own text, `#if`/`#endif` included, so the
// `#define` sits inside the arm and fires exactly when the pragma would have,
// while the `#ifndef` that skips a repeat inclusion stays at the top of the
// body.  Reaching it took two changes: `PragmaOperator` is admitted to the
// guard-rewrite authority, with line ownership decided per site rather than per
// header, and the site inventory is re-proven in the domain of the occurrence
// the producer *entered*.
//
// That second one is what the operator spelling exposed.  Both occurrences are
// materialized here -- a surviving `#include` would need an eager define, which
// a conditional site cannot license -- and the second occurrence was suppressed
// by the very once-state being catalogued, so it entered nothing and carries no
// producer record.  Re-proving in its own domain made every once site in it
// look unbound and rejected the header; the inventory belongs to the header's
// bytes, and only the binding evidence belongs to an occurrence.
//
// The arm is `#if 1`, always taken -- and that is the point.  Nothing here
// evaluates the condition; the define is placed inside the arm precisely so
// that it does not have to, because deciding otherwise means constant-folding
// preprocessor conditions to justify moving preprocessor state.
#include "guard_once_operator_midline_conditional.h"

int mid = 0;

#include "guard_once_operator_midline_conditional.h"

int tail = MIDLINE_COND_V;
