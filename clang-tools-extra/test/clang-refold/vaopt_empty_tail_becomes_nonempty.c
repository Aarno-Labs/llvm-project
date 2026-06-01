// RUN: %clang-refold-tester-with-lines vaopt_empty_tail_becomes_nonempty
#define MAYBE_PLUS(a, ...) int x = a __VA_OPT__(+ __VA_ARGS__);
MAYBE_PLUS(1)
