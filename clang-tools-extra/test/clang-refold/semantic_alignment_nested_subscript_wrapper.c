// RUN: %clang-refold-tester-with-lines semantic_alignment_nested_subscript_wrapper
// Regression: repeated comma and bracket tokens must preserve the argument
// partition through a nested index macro and two forwarding wrappers.
int prefix_marker = 17;
int lookup_table[100];
#define INDEX_SUM(a, b) a + b
#define EMIT_SUM3(a, b, c) int nested_result = a + b + c;
#define FORWARD_SUM3(a, b, c) EMIT_SUM3(a, b, c)
#define OUTER_SUM3(a, b, c) FORWARD_SUM3(a, b, c)
void build_nested(void) {
  OUTER_SUM3(lookup_table[INDEX_SUM(1, 2), 3], 4)
}
int suffix_marker = 23;
