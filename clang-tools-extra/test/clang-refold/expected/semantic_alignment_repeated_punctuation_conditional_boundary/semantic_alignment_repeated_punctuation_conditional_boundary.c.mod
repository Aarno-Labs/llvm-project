// RUN: %clang-refold-tester semantic_alignment_repeated_punctuation_conditional_boundary
// Repeated parentheses and operators must not move a partial macro edit across
// a protected conditional boundary or into the identical outside invocation.
#define DUP_SUM(x) ((x) + (x))

#if 1
int conditional_value = ((5) + (6));
#endif
int boundary_marker = 909;
int outside_value = DUP_SUM(5);
