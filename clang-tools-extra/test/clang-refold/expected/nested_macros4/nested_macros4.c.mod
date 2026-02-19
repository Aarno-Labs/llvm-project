// RUN: %clang-refold-tester nested_macros4
#define STR(X) #X
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define WSTR(X) WIDEN(STR(X))

const wchar_t *s = WSTR(goodbye); // L"hello"
