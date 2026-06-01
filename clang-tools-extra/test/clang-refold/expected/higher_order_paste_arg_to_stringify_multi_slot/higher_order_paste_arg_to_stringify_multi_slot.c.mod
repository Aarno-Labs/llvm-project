// RUN: %clang-refold-tester-with-lines higher_order_paste_arg_to_stringify_multi_slot
#define APPLY(F, G, A, B) F(G, A, B)
#define FWD_JOIN_ARG(G, A, B) G(A##B)
#define STR(x) #x

const char *s = APPLY(FWD_JOIN_ARG, STR, gamma, beta);
