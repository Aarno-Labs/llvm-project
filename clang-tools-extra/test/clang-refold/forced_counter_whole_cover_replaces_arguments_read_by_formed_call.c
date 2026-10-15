// RUN: %clang-refold-tester-verify-off forced_counter_whole_cover_replaces_arguments_read_by_formed_call
// Regression: B edits the first `__COUNTER__`, so the later read in DO_TW's
// body must be forced to B's value by a whole cover of `GET(TW)`.  That forced
// cover replaced only the callsite, though the call `DO_TW` its expansion
// formed reads `(10)` after it, so `(10)` followed the realized call.  The
// forced cover now replaces what its plan says the expansion read.
int twice(int v) { return 2 * v; }
#define GET(k) DO_##k
#define DO_TW(x) twice(x + __COUNTER__)
int z = __COUNTER__;
int a = GET(TW)(10);
