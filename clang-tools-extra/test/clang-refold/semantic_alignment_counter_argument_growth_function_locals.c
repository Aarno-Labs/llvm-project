// RUN: %clang-refold-tester semantic_alignment_counter_argument_growth_function_locals
// Regression: the argument-local counter alignment must remain valid when the
// same ambiguous expansion appears in a function-local declaration sequence.
#define ADD_LOCAL_COUNT(x, y) ((x) + (y))
#define LOCAL_COUNTED(x) ADD_LOCAL_COUNT(x, __COUNTER__)
int compute_local_growth(void) {
  int local_first = LOCAL_COUNTED(0);
  int local_middle = LOCAL_COUNTED(5);
  int local_last = LOCAL_COUNTED(10);
  return local_first + local_middle + local_last;
}
