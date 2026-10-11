// RUN: %clang-refold-tester object_alias_call_source_arguments_refold
// A call named by an object-like alias reads its whole argument list from the
// source after the alias: `OBJ` expands to `ADD`, which reads `(1, 2)`.  ADD's
// recorded invocation text runs from its name in OBJ's `#define` to the `)`,
// so it is not a callsite, but both of its actuals are source-spelled.  B
// edits `b`, and `OBJ(1, 7)` keeps both macros.
#define ADD(a, b) a + b
#define OBJ ADD
int k = OBJ(1, 7);
