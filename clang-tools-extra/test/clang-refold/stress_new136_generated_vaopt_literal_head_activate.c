// RUN: %clang-refold-tester-with-lines stress_new136_generated_vaopt_literal_head_activate
#define CALL(f, ...) f("tag" __VA_OPT__(,) __VA_ARGS__)
#define LOG(fmt, ...) emit(fmt __VA_OPT__(,) __VA_ARGS__)

CALL(LOG, A)
