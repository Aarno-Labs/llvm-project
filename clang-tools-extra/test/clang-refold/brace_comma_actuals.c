// RUN: %clang-refold-tester-with-lines brace_comma_actuals
#define MAKE_ARRAY(a, b) int arr[] = a, b;
MAKE_ARRAY({1, 2})
