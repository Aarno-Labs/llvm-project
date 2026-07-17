// RUN: %clang-refold-tester-with-lines stress_tuple_stringify_standard_parenthesized_comma
#define CALL(f, x) f(x)
#define WRAP(pair) CALL pair
#define F(x) use(#x, x)

int x = WRAP((F, pair(3, 4)));
