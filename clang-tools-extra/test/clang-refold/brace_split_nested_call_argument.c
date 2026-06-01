// RUN: %clang-refold-tester-with-lines brace_split_nested_call_argument
#define PAIR_EXPR(a, b) a + b
#define MAKE_ARRAY(a, b) int arr[] = a, b;
#define BUILD_ARRAY(a, b) MAKE_ARRAY(a, b)
#define WRAP_ARRAY(a, b) BUILD_ARRAY(a, b)

WRAP_ARRAY({PAIR_EXPR(1, 2), 3})
