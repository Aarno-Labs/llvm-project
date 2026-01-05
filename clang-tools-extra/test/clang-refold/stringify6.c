// RUN: %clang-refold-tester stringify6
#define COMMA ,
#define STR(...) #__VA_ARGS__           // Can take any number of args
#define XSTR(...) STR(__VA_ARGS__)      // Passes all args to STR

// Result: "int a , int b"
const char* s = XSTR(int a COMMA int b);
