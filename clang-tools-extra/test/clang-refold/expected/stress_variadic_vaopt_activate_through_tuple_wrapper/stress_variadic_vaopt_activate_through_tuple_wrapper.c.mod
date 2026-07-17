// RUN: %clang-refold-tester-with-lines stress_variadic_vaopt_activate_through_tuple_wrapper
#define LOG(fmt, ...) emit(fmt __VA_OPT__(,) __VA_ARGS__)
#define WRAP(args) LOG args

WRAP(("x", A, B));
