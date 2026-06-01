// RUN: %clang-refold-tester-with-lines higher_order_nested_call_arg_multi_slot_stringify
#define APPLY(F, H, G, A, B) F(H, G, A, B)
#define FWD_COMPOSE2(H, G, A, B) H(G(A, B))
#define STR(x) #x
#define PAIR(a, b) a + b

const char *s = APPLY(FWD_COMPOSE2, STR, PAIR, gamma, beta);
