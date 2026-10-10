// RUN: %clang-refold-tester-verify-off whole_cover_replaces_source_arguments_of_generated_call
// The source a generated call consumes after the callsite can hold arguments as
// well as its closing parenthesis.  `CALL(ADD, 1)` leaves `ADD(1` open, and
// `, 2)` completes it; B edits the `2`.  Its realization must replace the
// argument it realized: it used to emit `int k = 1 + 7, 2);` at verify-off.
#define ADD(a, b) a + b
#define CALL(f, a) int k = f(a
int k = 1 + 7;
