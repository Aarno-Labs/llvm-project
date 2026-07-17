// RUN: %clang-refold-tester-with-lines stress_nested_generated_vaopt_paste_payload_activate
#define CALL(f, ...) f(__VA_ARGS__)
#define MAKE(prefix, ...) prefix ## __VA_OPT__(__VA_ARGS__)

int x = CALL(MAKE, foo);
