// RUN: %clang-refold-tester-with-lines higher_order_decorated_join_arg_two_slots_paste_callee
#define APPLY(F, G, A, B) F(G, A, B)
#define FWD_DECOR(G, A, B) G(pre_##A##_mid_##B##_suf)
#define MAKE(x) v_##x

int APPLY(FWD_DECOR, MAKE, alpha, beta) = 1;
