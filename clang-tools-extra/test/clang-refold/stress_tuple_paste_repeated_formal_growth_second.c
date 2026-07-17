// RUN: %clang-refold-tester-with-lines stress_tuple_paste_repeated_formal_growth_second
#define CALL(f, t) f t
#define WRAP(pair) CALL pair
#define F(x, y) x##y + y

int x = WRAP((F, (ab, c)));
