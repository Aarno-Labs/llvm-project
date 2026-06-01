// RUN: %clang-refold-tester-with-lines empty_actual_long_insertion_exceeds_replay_cap
#define EXPR(a, b) int x = a b;

EXPR(0, )
