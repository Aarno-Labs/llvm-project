// RUN: %clang-refold-tester-with-lines higher_order_paste_arg_to_paste_callee_multi_slot
#define APPLY(F, G, A, B) F(G, A, B)
#define FWD_JOIN_ARG(G, A, B) G(A##B)
#define MAKE(x) pre_##x##_suf

int APPLY(FWD_JOIN_ARG, MAKE, qux, bar) = 1;
