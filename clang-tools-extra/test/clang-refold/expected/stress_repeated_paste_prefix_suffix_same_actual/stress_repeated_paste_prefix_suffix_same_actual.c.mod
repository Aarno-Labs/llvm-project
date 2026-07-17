// RUN: %clang-refold-tester-with-lines stress_repeated_paste_prefix_suffix_same_actual
#define MIRROR(x) x##L + R##x

int v = MIRROR(bar);
