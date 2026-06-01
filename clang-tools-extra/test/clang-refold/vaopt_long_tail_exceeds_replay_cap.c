// RUN: %clang-refold-tester-with-lines vaopt_long_tail_exceeds_replay_cap
#define LIST(a, ...) int arr[] = { a __VA_OPT__(, __VA_ARGS__) };

LIST(0)
