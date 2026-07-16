// RUN: %clang-refold-tester-with-lines stress_nested_tuple_forwarder_vaopt_deactivate
#define LOG(fmt, ...) emit(fmt __VA_OPT__(,) __VA_ARGS__)
#define CALL(f, t) f t
#define OUTER(pair) CALL pair

OUTER((LOG, ("x", A, B)));
