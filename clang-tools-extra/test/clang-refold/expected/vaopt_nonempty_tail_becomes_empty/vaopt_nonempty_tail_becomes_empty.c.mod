// RUN: %clang-refold-tester-with-lines vaopt_nonempty_tail_becomes_empty
#define MAYBE_PLUS(a, ...) int x = a __VA_OPT__(+ __VA_ARGS__);
MAYBE_PLUS(3)
