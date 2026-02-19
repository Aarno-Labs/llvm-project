// RUN: %clang-refold-tester stringify4
#define TO_STR(...) #__VA_ARGS__

// Result: "int a, int b"
const char* s = TO_STR(int x, int y);
