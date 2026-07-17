// RUN: %clang-refold-tester-with-lines stress_generated_vaopt_tail_literal_payload_activate
#define CALL(f, ...) f(__VA_ARGS__)
#define LOG(...) emit(__VA_ARGS__ __VA_OPT__(, done))

CALL(LOG, A)
