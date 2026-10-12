// RUN: %clang-refold-tester-verify-off whole_cover_consumed_source_counter_read_forces_later_reads
// Regression: a whole cover replaces every source byte its expansion consumed,
// including the `__COUNTER__ + 2)` that ADD reads past CALL's closing
// parenthesis.  That read leaves the replay-time counter sequence, so the later
// `__COUNTER__` must be forced to B's value instead of reading 0.  B edits
// CALL's replacement-list `int`, which only an expansion realizes.
#define ADD(a, b) a + b
#define CALL(f, a) int k = f(a
CALL(ADD, 1), __COUNTER__ + 2);
int c = __COUNTER__;
