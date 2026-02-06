// RUN: %clang-refold-tester stringify_var_args1
#define TO_STR(...) #__VA_ARGS__

// FIXME: should not cause expansion, but at laast we shouldn't break anything.
const char* s = TO_STR(int a, int b);
