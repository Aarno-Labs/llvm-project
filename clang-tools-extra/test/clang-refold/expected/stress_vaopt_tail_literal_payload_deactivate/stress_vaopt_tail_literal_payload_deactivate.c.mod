// RUN: %clang-refold-tester-with-lines stress_vaopt_tail_literal_payload_deactivate
#define LOG(...) emit(__VA_ARGS__ __VA_OPT__(, done))

LOG()
