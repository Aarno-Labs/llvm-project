// RUN: %clang-refold-tester assert_stale_stringified_arg_strict
//
// Strict companion to assert_stale_stringified_arg_relaxed.c (same edit).
//
// The `assert`-style macro uses its argument both evaluated `(expr)` and
// stringified `#expr`.  The modified stream edits only the evaluated
// expression (`x == z || x == y`, plus an inserted `int z = 9;`) and leaves the
// stringified copy stale as `"x == y"`.
//
// In strict mode the stringified occurrence no longer matches the edited
// argument, so folding back to `assert(...)` would silently rewrite the assert
// message.  Strict refuses that and instead preserves the modified stream
// exactly by EXPANDING the macro.  The expansion reproduces B byte for byte, so
// the standard re-preprocess check passes.
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
assert(x == y);
