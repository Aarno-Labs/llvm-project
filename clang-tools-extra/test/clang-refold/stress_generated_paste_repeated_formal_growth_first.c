// RUN: %clang-refold-tester-with-lines stress_generated_paste_repeated_formal_growth_first
#define CALL(f, a, b) f(a, b)
#define F(x, y) x##y + x

int x = CALL(F, a, bc);
