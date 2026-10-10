// RUN: %clang-refold-tester-verify-off recursive_tuple_generated_callee_edited_element_copy_refolds
// Control for recursive_tuple_generated_callee_keeps_stale_element_copy.c: a
// copy of an edited terminal actual that B spells as the solved actual is
// proven, so the recursive tuple replay still folds.  No other path folds this
// shape, and refusing every copy outside the replayed surfaces expands it.
#define ADD(a, b) int a; int b;
#define FIRST(a, b) a
#define CALL(F, PAIR) F PAIR int k = FIRST PAIR;
CALL(ADD, (y, w))
