// RUN: %clang-refold-tester-relaxed-expect-refold-fail assert_independent_stringified_arg_relaxed_check_rejects
// Soundness guard for the relaxed stringify rule (negative assertion).
//
// Same `assert` shape as assert_stale_stringified_arg_relaxed.c, but here the
// modified stream edits BOTH the evaluated expression (to `x == z || x == y`)
// AND the stringified copy -- to an independent value `"assertion failed"` that
// is NOT the stale original `"x == y"`.
//
// The relaxed pipeline still folds `assert(x == z || x == y)` from the
// authoritative evaluated occurrence.  Re-preprocessing that fold regenerates
// the message as `"x == z || x == y"`, which matches neither the original nor
// B's independent `"assertion failed"`.  Because B did NOT keep the original
// stringification, that position is not a provably stale observer, so the fold
// is not sound and must not be emitted.
//
// This pins the "tolerate iff stale" rule: the relaxation must not degrade into
// ignoring every stringified position.  The refusal is asserted at the point of
// production -- the closing verification refuses to emit -- rather than by
// letting the source be written and rejected afterwards.  The diagnosis is
//
//   clang-refold: refolded source does not replay the edited preprocessed
//   stream ... --verify-output=repair
//
// recorded here rather than FileCheck'd, because a RUN line placed after a
// failing one never runs.
//
// PINNED BY ASSERTION, NOT BY `XFAIL`.  The harness requires the refold to
// fail, so this reports as a passing test.  A shape that must never fold is not
// unfinished work, and an expected-failure entry would say it is -- and would
// sit in the remaining-work count forever.  If the shape ever starts folding
// this test fails, which is the same alarm `XPASS` used to raise.  The expected
// refold beside this file is no longer diffed; it stays as the record of what
// admitting it would have produced.  The `.c.i` still is, so producer drift is
// still caught.
//
// TRIAGE: correct.  THIS TEST MUST NOT BE MADE TO PASS.  No source realizes B:
// a single macro argument cannot both evaluate to `x == z || x == y` and
// stringify to `"assertion failed"`.  The expected refold below is the fold the
// relaxed pipeline computes and then refuses -- it is recorded so the refusal
// can be seen to be about the message rather than about the expression, and it
// deliberately does NOT preprocess to B.  If this test ever stops failing,
// something has started tolerating an independently edited stringification.
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
assert(x == y);
