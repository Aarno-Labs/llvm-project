// RUN: %clang-refold-tester-verify-off generated_call_source_argument_with_root_body_edit_expands
// Rewriting a generated call's source-spelled actual in place keeps the root's
// callsite, so it is admitted only when it explains every edit to the root's
// cover.  B edits CALL's replacement-list `int` as well as ADD's `b`.  Only an
// expansion realizes the first, and it already replaces the `, 2)` that ADD
// read, so the second must not be realized beside it.
#define ADD(a, b) a + b
#define CALL(f, a) int k = f(a
long k = 1 + 7;
