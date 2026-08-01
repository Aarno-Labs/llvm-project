// RUN: %clang-refold-tester-relaxed assert_stale_stringified_arg_relaxed
//
// Relaxed refold of an `assert`-style macro whose argument is used BOTH
// evaluated `(expr)` and stringified `#expr`.
//
// The modified stream edits the evaluated expression (inserts `int z = 9;` and
// grows the condition to `x == z || x == y`) but leaves the stringified copy as
// the stale `"x == y"`.  The two occurrences of the formal therefore disagree.
//
// In relaxed (non-strict) mode the evaluated occurrence is authoritative: the
// pipeline folds the edit back to `assert(x == z || x == y)` and tolerates the
// stale `#expr`, because re-expanding the refolded source regenerates the
// stringification from the new argument.  Re-preprocessing the refold therefore
// yields the message `"x == z || x == y"`, not B's stale `"x == y"`.  The
// relaxed `--check` verifies this: it tolerates the difference precisely because
// B kept the original stringification (a provably stale observer), while still
// requiring an exact match everywhere else -- an independently edited `#expr`
// would fail.
//
// Strict mode instead preserves the modified stream exactly by expanding the
// macro; see assert_stale_stringified_arg_strict.c for that companion.
//
// Regression: before the occurrence-collector fix a stale stringified
// occurrence forced tuple-forwarding and both modes expanded, so the relaxed
// stringify relaxation was dead.
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
assert(x == y);
