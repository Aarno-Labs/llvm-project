// RUN: %clang-refold-tester-with-lines higher_order_decorated_join_arg_two_slots_stringify
#define APPLY(F, G, A, B) F(G, A, B)
#define FWD_DECOR(G, A, B) G(pre_##A##_mid_##B##_suf)
#define STR(x) #x

const char *s = APPLY(FWD_DECOR, STR, alpha, beta);
