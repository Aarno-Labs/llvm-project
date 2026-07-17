// RUN: %clang-refold-tester-with-lines stress_vaopt_stringify_payload_deactivate
#define S(prefix, ...) prefix __VA_OPT__(, #__VA_ARGS__)

const char *a[] = { S("p") };
