// RUN: %clang-refold-tester-with-lines brace_split_wrapper_chain
#define MAKE_ARRAY(a, b) int arr[] = a, b;
#define BUILD_ARRAY(a, b) MAKE_ARRAY(a, b)
#define WRAP_ARRAY(a, b) BUILD_ARRAY(a, b)

WRAP_ARRAY({1, 2})
