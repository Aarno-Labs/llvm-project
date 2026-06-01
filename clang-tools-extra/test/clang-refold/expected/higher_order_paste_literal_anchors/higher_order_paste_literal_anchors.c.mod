// RUN: %clang-refold-tester-with-lines higher_order_paste_literal_anchors
#define APPLY(F, G, X) F(G, X)
#define FWD(G, X) G(X)
#define MAKE(name) pre_##name##_suf

int APPLY(FWD, MAKE, longer) = 1;
