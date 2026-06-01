// RUN: %clang-refold-tester-with-lines higher_order_paste_generated_argument_to_stringify
// 03_higher_order_paste_generated_argument_to_stringify
#define APPLY(F, G, X) F(G, X)
#define FWD_PASTE_ARG(G, X) G(pre_##X)
#define STR(x) #x

const char *s = APPLY(FWD_PASTE_ARG, STR, beta);
