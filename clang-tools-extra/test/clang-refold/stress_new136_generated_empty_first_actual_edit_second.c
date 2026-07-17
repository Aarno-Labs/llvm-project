// RUN: %clang-refold-tester-with-lines stress_new136_generated_empty_first_actual_edit_second
#define CALL(f, t) f t
#define SECOND(a, b) b

int x = CALL(SECOND, (, 1));
