// RUN: %clang-refold-tester-with-lines higher_order_paste_actuals
#define APPLY(F, G, A, B) F(G, A, B)
#define FWD2(G, A, B) G(A, B)
#define CAT(a, b) a##b

int APPLY(FWD2, CAT, foo, bar) = 1;
