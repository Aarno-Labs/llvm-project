// RUN: %clang-refold-tester-relaxed-verify-off tuple_forwarded_actual_stale_stringified_observer_relaxed
// Control: the relaxed pipeline keeps its stale-stringification tolerance on
// the tuple-forwarded args-only path.
//
// B renames both elements but leaves `#PAIR` at "(x, z)".  Relaxed folds
// `CALL(ADD, (y, w))` and its check masks the stale literal, exactly as on the
// standard path (assert_stale_stringified_arg_relaxed.c).  Strict refuses the
// same edit: tuple_forwarded_actual_keeps_stringified_observer.c.
#define ADD(a, b) int a; int b;
#define CALL(F, PAIR) F PAIR char *s = #PAIR;
CALL(ADD, (y, w))
