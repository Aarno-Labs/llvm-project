// RUN: %clang-refold-tester-verify-off tuple_forwarded_actual_reproduces_stringified_observer
// Control for tuple_forwarded_actual_keeps_stringified_observer.c: when B's
// `#PAIR` is what the rebuilt actual stringifies to, the tuple-forwarded
// rewrite still folds.  Only that path folds this shape; refusing every
// tuple-forwarded rewrite expands it.
#define ADD(a, b) int a; int b;
#define CALL(F, PAIR) F PAIR char *s = #PAIR;
CALL(ADD, (x, z))
