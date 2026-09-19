// RUN: %clang-refold-tester-relaxed-verify-off assert_independent_stringified_arg_relaxed_planner_expands
// Soundness guard for the relaxed stringify rule.
//
// Same `assert` shape as assert_stale_stringified_arg_relaxed.c, but here the
// modified stream edits BOTH the evaluated expression (to `x == z || x == y`)
// AND the stringified copy -- to an independent value `"assertion failed"` that
// is NOT the stale original `"x == y"`.
//
// The relaxed pipeline tolerates a stringified occurrence that disagrees with
// the argument an evaluated occurrence fixed, because re-expanding the refolded
// invocation regenerates `#expr` from the new argument.  That licence holds
// only for a *stale* occurrence -- one B left at its original spelling, which
// therefore records no edit.  Here B rewrote the literal independently, so
// folding to `assert(x == z || x == y)` would regenerate the message as
// `"x == z || x == y"`: neither the original nor B's, and the edit at that
// position would be silently discarded.
//
// So the args-only candidate is refused and the ladder realizes the callsite by
// whole cover, which reproduces B exactly.  The `#define` survives, unused at
// this site; only the fold is given up.
//
// This is a --verify-output=off test on purpose, and the assertion is the
// emitted source.  Under `fatal` the closing check re-preprocesses the output
// and refuses the bad fold itself, so the test would pass whether or not the
// admission rule holds -- which is how this shape previously reached the
// caller: at the tool's default it exited 0 and emitted the corrupted message.
// Off, nothing rescues the planner, so the pinned `.c.mod` is a statement about
// the admission rule.
//
// The repair counterpart is
// assert_independent_stringified_arg_repair_mode_matches_planner.c, which
// drives the same A/B through --verify-output=repair and must agree.
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
int z = 9;
((x == z || x == y) ? (void)0 : __assert_fail("assertion failed"));
