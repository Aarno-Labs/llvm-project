// RUN: %clang-refold-tester-with-lines higher_order_parenthesized_nested_source_arg
// 02_higher_order_parenthesized_nested_source_arg
#define APPLY(F, G, X) F(G, X)
#define FWD_PAREN(G, X) G((X))
#define STR(x) #x
#define ID(x) x

const char *s = APPLY(FWD_PAREN, STR, ID(alpha));
