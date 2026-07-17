// RUN: %clang-refold-tester-with-lines stress_generated_paste_stringify_same_actual
#define CALL(f, x) f(x)
#define F(x) int x##_value = 0; const char *name = #x;

CALL(F, bar)
