// RUN: %clang-refold-tester-verify-off whole_cover_header_counter_read_forces_later_reads
// Regression: a whole cover of the header's F replaces its callsite, and with
// it the `__COUNTER__` spelled in F's actual.  That read leaves the replay-time
// counter sequence, so the translation unit's later `__COUNTER__` must be
// forced to B's value instead of reading 0, and the removed read itself must
// not be forced as well: a literal nested inside the header's whole cover
// would overlap it.  B edits F's replacement-list `2`, which only an expansion
// realizes.
#define F(x) int k = x + 2;
// Header for whole_cover_header_counter_read_forces_later_reads.c.
int k = 0 + 7;
int c = 1;
