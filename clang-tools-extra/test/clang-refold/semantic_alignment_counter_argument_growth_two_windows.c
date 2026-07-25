// RUN: %clang-refold-tester semantic_alignment_counter_argument_growth_two_windows
// Regression: two independent counter-growth ambiguity windows must compose to
// the unique byte-exact source-minimal realization without exceeding proof scope.
#define ADD(x, y) ((x) + (y))
#define LEFT_COUNTED(x) ADD(x, __COUNTER__)
#define RIGHT_COUNTED(x) ADD(__COUNTER__, x)
int left_first = LEFT_COUNTED(0);
int left_middle = LEFT_COUNTED(5);
int left_last = LEFT_COUNTED(10);
int semantic_window_separator = 909;
int right_first = RIGHT_COUNTED(0);
int right_middle = RIGHT_COUNTED(5);
int right_last = RIGHT_COUNTED(10);
