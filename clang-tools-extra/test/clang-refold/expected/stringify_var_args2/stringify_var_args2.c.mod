// RUN: %clang-refold-tester stringify_var_args2
#define STR(...) #__VA_ARGS__
#define SHIELD_STR(x) STR x

// FIXME: should not cause expansion, but at laast we shouldn't break anything.
const char* s = "int x, int y";
