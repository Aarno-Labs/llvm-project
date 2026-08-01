// RUN: %clang-refold-tester-with-lines recursive_tuple_generated_callee_two_identical_callees_refolds
// Both forwarded callees are the same macro ADD, so two root-tuple slices spell
// "ADD". This is unambiguous for the refold: each terminal's args come from the
// shared tuple (t) by exact source-range identity. The arg edit (1,2)->(10,5)
// must refold structure-preserving.
#define ADD(a, b) ((a) + (b))
#define BOTH(f, g, t) f t + g t
#define OUTER(p) BOTH p
int x = OUTER((ADD, ADD, (10, 5)));
