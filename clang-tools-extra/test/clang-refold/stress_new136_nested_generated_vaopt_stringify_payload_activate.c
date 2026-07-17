// RUN: %clang-refold-tester-with-lines stress_new136_nested_generated_vaopt_stringify_payload_activate
#define CALL(f, ...) f(__VA_ARGS__)
#define S(prefix, ...) prefix __VA_OPT__(# __VA_ARGS__)

const char *s = CALL(S, "p");
