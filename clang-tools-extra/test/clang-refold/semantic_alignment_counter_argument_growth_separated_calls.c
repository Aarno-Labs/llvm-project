// RUN: %clang-refold-tester semantic_alignment_counter_argument_growth_separated_calls
// Regression: unrelated TU-owned declarations between counter-bearing calls
// must not change the argument-local solution of the repeated-token window.
#define ADD_SEPARATED_COUNT(x, y) ((x) + (y))
#define SEPARATED_COUNTED(x) ADD_SEPARATED_COUNT(x, __COUNTER__)
int separated_first = SEPARATED_COUNTED(0);
int separator_alpha = 301;
int separated_middle = SEPARATED_COUNTED(5);
int separator_beta = 302;
int separated_last = SEPARATED_COUNTED(10);
