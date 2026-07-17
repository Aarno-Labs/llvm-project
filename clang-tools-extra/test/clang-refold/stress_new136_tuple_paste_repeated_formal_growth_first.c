// RUN: %clang-refold-tester-with-lines stress_new136_tuple_paste_repeated_formal_growth_first
#define CALL(f, t) f t
#define WRAP(pair) CALL pair
#define F(x, y) x##y + x

int x = WRAP((F, (a, bc)));
