// RUN: %clang-refold-tester-with-lines stress_two_terminal_tuple_standard_and_paste_width_growth
#define USE(a, b) a##b + a
#define VALUE(a, b) a
#define BOTH(t) USE t + VALUE t

int x = BOTH((xyz, bc));
