// RUN: %clang-refold-tester-with-lines recursive_tuple_generated_callee_repeated_callee_name_still_refolds
// A decoy tuple element (h) repeats the callee name ADD but is unused by BOTH's
// body. The opaque-callee recovery matches root-tuple slice text to the callee
// name, so two slices spell "ADD" -- but that is NOT ambiguous: a callee-name
// slice is a bare identifier, never a tuple, so it can never be the args source.
// The args (t) remain uniquely determined, so the tuple-arg edit (1,2)->(10,5)
// must refold structure-preserving, not fall back to whole-cover expansion.
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))
#define BOTH(f, g, h, t) f t + g t
#define OUTER(p) BOTH p
int x = OUTER((ADD, SUB, ADD, (10, 5)));
