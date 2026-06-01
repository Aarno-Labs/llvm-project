// RUN: %clang-refold-tester-with-lines higher_order_argument_literal_suffix_to_paste
// 08_higher_order_argument_literal_suffix_to_paste
#define APPLY(F, G, X) F(G, X)
#define FWD_SUFFIX_ARG(G, X) G(X##_tail)
#define MAKE(name) pre_##name

int APPLY(FWD_SUFFIX_ARG, MAKE, alpha) = 1;
