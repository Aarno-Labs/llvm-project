// RUN: %clang-refold-tester-verify-off whole_cover_actual_counter_read_forces_later_reads
// Regression: a whole cover of F replaces its callsite, and with it the
// `__COUNTER__` spelled in F's actual.  That read leaves the replay-time counter
// sequence, so the later `__COUNTER__` must be forced to B's value instead of
// reading 0.  B edits F's replacement-list `2`, which only an expansion
// realizes.
#define F(x) int k = x + 2;
int k = 0 + 7;
int c = 1;
