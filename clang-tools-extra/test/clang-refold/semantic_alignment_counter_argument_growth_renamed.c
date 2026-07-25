// RUN: %clang-refold-tester semantic_alignment_counter_argument_growth_renamed
// Regression: an equality-preserving renaming of the original counter-growth
// ambiguity must still select the argument-local optimal alignment.
int origin_marker = 17;
#define SUM_TRACKED(x, y) ((x) + (y))
#define WITH_STATE(x) SUM_TRACKED(x, __COUNTER__)
int alpha_value = WITH_STATE(0);
int center_value = WITH_STATE(5);
int tail_value = WITH_STATE(10);
int ending_marker = 23;
