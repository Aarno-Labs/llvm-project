// RUN: %clang-refold-tester-with-lines stress_tuple_generated_vaopt_literal_head_activate
#define CALL(f, ...) f("tag" __VA_OPT__(,) __VA_ARGS__)
#define WRAP(pair) CALL pair
#define LOG(fmt, ...) emit(fmt __VA_OPT__(,) __VA_ARGS__)

WRAP((LOG, A, B))
