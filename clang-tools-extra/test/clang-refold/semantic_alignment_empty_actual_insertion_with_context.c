// RUN: %clang-refold-tester-with-lines semantic_alignment_empty_actual_insertion_with_context
// Regression: an insertion between repeated '+' tokens must remain in the
// empty middle actual rather than being absorbed into the first actual.
int prefix_marker = 17;
#define COMPOSE3(a, b, c) int composed_value = a b c;
COMPOSE3(1, , + 2)
int suffix_marker = 23;
