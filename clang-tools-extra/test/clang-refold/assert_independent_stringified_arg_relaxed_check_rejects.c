// RUN: %clang-refold-tester-relaxed-check-rejects assert_independent_stringified_arg_relaxed_check_rejects
//
// Soundness guard for the relaxed stringify check (negative assertion).
//
// Same `assert` shape as assert_stale_stringified_arg_relaxed.c, but here the
// modified stream edits BOTH the evaluated expression (to `x == z || x == y`)
// AND the stringified copy -- to an independent value `"assertion failed"` that
// is NOT the stale original `"x == y"`.
//
// The relaxed pipeline still folds `assert(x == z || x == y)` from the
// authoritative evaluated occurrence, so the refolded source matches the
// expected .c.mod.  But re-preprocessing that fold regenerates the message as
// `"x == z || x == y"`, which matches neither the original nor B's independent
// `"assertion failed"`.  Because B did NOT keep the original stringification,
// that position is not a provably stale observer, so the relaxed --check must
// REJECT the fold (it would otherwise silently drop B's intended message).
//
// This pins the "tolerate iff stale" rule: the check must not degrade into
// ignoring every stringified position.
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
assert(x == y);
