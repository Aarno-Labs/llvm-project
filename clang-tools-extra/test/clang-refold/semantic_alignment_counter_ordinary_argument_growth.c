// RUN: %clang-refold-tester semantic_alignment_counter_ordinary_argument_growth
// Regression: ordinary argument edits must preserve caller form and the exact
// __COUNTER__ consumption schedule when the builtin itself is untouched.
int prefix_marker = 17;
#define ADD_COUNTED(x, y) ((x) + (y))
#define WITH_COUNT(x) ADD_COUNTED(x, __COUNTER__)
int first_value = WITH_COUNT(0);
int middle_value = WITH_COUNT(5);
int last_value = WITH_COUNT(10);
int suffix_marker = 23;
