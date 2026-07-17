// RUN: %clang-refold-tester-with-lines stress_vaopt_paste_payload_deactivate
#define MAKE(base, ...) base ## __VA_OPT__(__VA_ARGS__)

int x = MAKE(foo, bar);
