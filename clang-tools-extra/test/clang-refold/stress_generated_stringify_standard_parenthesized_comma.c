// RUN: %clang-refold-tester-with-lines stress_generated_stringify_standard_parenthesized_comma
#define CALL(f, x) f(x)
#define F(x) use(#x, x)

int x = CALL(F, pair(1, 2));
