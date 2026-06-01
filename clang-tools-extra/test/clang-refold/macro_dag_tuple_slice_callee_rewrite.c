// RUN: %clang-refold-tester-with-lines macro_dag_tuple_slice_callee_rewrite
// Step 5 regression: the callee token for ADD_ONE is forwarded through
// the first slice of WRAP's tuple argument.  Editing only ADD_ONE's ordinary
// argument should rewrite the tuple element and preserve both macro layers.
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR
#define ADD_ONE(x) ((x) + 1)

int value = WRAP((ADD_ONE, 10));
