// RUN: %clang-refold-tester-verify-off generated_call_source_argument_split_by_comma_expands
// A rewritten source-spelled actual must stay one actual.  ADD reads `b` from
// the source after CALL's callsite and prints it first.  B replaces it with
// `5, j = 7`, which declares a second variable; written as `b` it would give
// ADD three actuals.  No spelling of ADD's actuals prints a top-level comma
// before `+ 1`, so the callsite is expanded.
#define ADD(a, b) b + a
#define CALL(f, a) int k = f(a
CALL(ADD, 1), 2);
