// RUN: %clang-refold-tester-with-lines stress_generated_paste_repeated_formal_growth_second
#define CALL(f, a, b) f(a, b)
#define F(x, y) x##y + y

int x = CALL(F, ab, qrst);
