// RUN: %clang-refold-tester-with-lines stress_deep_generated_paste_repeated_formal_growth
#define A(f, a, b) B(f, a, b)
#define B(g, x, y) g(x, y)
#define F(x, y) x##y + x

int x = A(F, a, bc);
