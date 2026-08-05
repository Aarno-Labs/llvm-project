// RUN: %clang-refold-tester-relaxed-expect-refold-fail assert_independent_stringified_arg_relaxed_check_rejects
// RUN: FileCheck %s --check-prefix=AUDIT < %t/outputs/assert_independent_stringified_arg_relaxed_check_rejects.out
//
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
// letting the source be written and rejected afterwards.
//
// AUDIT: clang-refold: refolded source does not replay the edited preprocessed stream
// AUDIT-SAME: --verify-output=repair
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
assert(x == y);
