// RUN: %clang-refold-tester stringify5
#define STR(...) #__VA_ARGS__
#define SHIELD_STR(x) STR x

// Works: "int a, int b"
const char* s = "int x, int y";
