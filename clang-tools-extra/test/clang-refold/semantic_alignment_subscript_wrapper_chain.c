// RUN: %clang-refold-tester-with-lines semantic_alignment_subscript_wrapper_chain
// Regression: a comma inside a subscript must not be reassigned to an outer
// formal while edits cross repeated '+' and ']' boundaries.
int prefix_marker = 17;
int lookup_table[32];
#define EMIT_TOTAL3(a, b, c) int wrapper_result = a + b + c;
#define FORWARD_TOTAL3(a, b, c) EMIT_TOTAL3(a, b, c)
#define OUTER_TOTAL3(a, b, c) FORWARD_TOTAL3(a, b, c)
void build_wrapper(void) {
  OUTER_TOTAL3(lookup_table[1, 2], 3)
}
int suffix_marker = 23;
