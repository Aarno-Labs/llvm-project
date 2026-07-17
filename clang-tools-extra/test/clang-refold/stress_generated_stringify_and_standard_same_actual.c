// RUN: %clang-refold-tester-with-lines stress_generated_stringify_and_standard_same_actual
#define CALL(f, x) f(x)
#define F(x) use(#x, x)

int x = CALL(F, alpha);
