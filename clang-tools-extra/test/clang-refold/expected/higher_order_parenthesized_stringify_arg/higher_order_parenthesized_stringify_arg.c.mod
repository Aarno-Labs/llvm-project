// RUN: %clang-refold-tester-with-lines higher_order_parenthesized_stringify_arg
// 01_higher_order_parenthesized_stringify_arg
#define APPLY(F, G, X) F(G, X)
#define FWD_PAREN(G, X) G((X))
#define STR(x) #x

const char *s = APPLY(FWD_PAREN, STR, beta);
