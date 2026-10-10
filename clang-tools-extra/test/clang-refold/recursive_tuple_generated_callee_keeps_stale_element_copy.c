// RUN: %clang-refold-tester-verify-off recursive_tuple_generated_callee_keeps_stale_element_copy
// The recursive tuple replay proves a terminal actual only inside the terminal
// expansion it replays; every other copy of the actual must spell the solved
// edit.
//
// `FIRST PAIR` copies the element `x`, and the producer attributes that copy to
// `ADD`'s argument span as well.  B renames both elements inside `ADD`'s
// expansion but keeps `k = x`.  The certificate took the whole span as solved,
// so this used to emit `CALL(ADD, (y, w))` at verify-off.
#define ADD(a, b) int a; int b;
#define FIRST(a, b) a
#define CALL(F, PAIR) F PAIR int k = FIRST PAIR;
CALL(ADD, (x, z))
