// RUN: %clang-refold-tester-with-lines higher_order_nested_call_arg_two_slots_stringify
#define APPLY(F, G, H, A, B) F(G, H, A, B)
#define FWD_COMPOSE2(G, H, A, B) G(H(A, B))
#define STR(x) #x
#define PAIR(a, b) a + b

const char *s = APPLY(FWD_COMPOSE2, STR, PAIR, gamma, delta);
