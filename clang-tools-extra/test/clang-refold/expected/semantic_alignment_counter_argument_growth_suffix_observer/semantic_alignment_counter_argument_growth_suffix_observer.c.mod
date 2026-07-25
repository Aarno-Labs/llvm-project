// RUN: %clang-refold-tester semantic_alignment_counter_argument_growth_suffix_observer
// Regression: preserving the edited calls must also preserve an untouched
// later counter-bearing call and its exact builtin-consumption position.
#define ADD_OBSERVED_COUNT(x, y) ((x) + (y))
#define OBSERVED_COUNTED(x) ADD_OBSERVED_COUNT(x, __COUNTER__)
int observed_first = OBSERVED_COUNTED(1);
int observed_middle = OBSERVED_COUNTED((6) + 4);
int observed_last = OBSERVED_COUNTED(11);
int observed_suffix = OBSERVED_COUNTED(15);
