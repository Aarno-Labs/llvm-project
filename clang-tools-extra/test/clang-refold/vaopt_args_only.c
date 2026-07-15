// RUN: %clang-refold-tester vaopt_args_only
// 1. Define the internal helpers for different argument counts
#define SEMI_LIST_1(a)          a
#define SEMI_LIST_2(a, b)       a; b
#define SEMI_LIST_3(a, b, c)    a; b; c
#define SEMI_LIST_4(a, b, c, d) a; b; c; d
// Add SEMI_LIST_5, etc., if you need to support more arguments

// 2. The argument counting mechanism
#define SEMI_LIST_GET_MACRO(_1, _2, _3, _4, NAME, ...) NAME

// 3. The public macro using __VA_OPT__
#define SEMI_LIST(...) __VA_OPT__( \
    SEMI_LIST_GET_MACRO(__VA_ARGS__, SEMI_LIST_4, SEMI_LIST_3, SEMI_LIST_2, SEMI_LIST_1)(__VA_ARGS__) \
)

SEMI_LIST(int x = 1, int y = 2, int z = 3);
