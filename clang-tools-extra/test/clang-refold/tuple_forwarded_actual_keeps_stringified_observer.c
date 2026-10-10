// RUN: %clang-refold-tester-verify-off tuple_forwarded_actual_keeps_stringified_observer
// A tuple-forwarded args-only rewrite must reproduce every stringification of
// the formal it rebuilds, not only the elements it copies.
//
// `PAIR` reaches `ADD` element by element, so B's `int y; int w;` rebuilds the
// actual as `(y, w)` slice by slice.  `#PAIR` observes the whole actual, and B
// kept it at "(x, z)", which `(y, w)` cannot regenerate.  The slice validator
// compared only the copies, so this used to emit `CALL(ADD, (y, w))` at
// verify-off.  The invocation is realized from B instead.
#define ADD(a, b) int a; int b;
#define CALL(F, PAIR) F PAIR char *s = #PAIR;
CALL(ADD, (x, z))
