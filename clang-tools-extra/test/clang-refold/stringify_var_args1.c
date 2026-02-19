// RUN: %clang-refold-tester stringify_var_args1
#define TO_STR(...) #__VA_ARGS__

const char* s = TO_STR(int a, int b);
