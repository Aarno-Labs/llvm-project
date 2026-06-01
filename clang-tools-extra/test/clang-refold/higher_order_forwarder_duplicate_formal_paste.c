// RUN: %clang-refold-tester-with-lines higher_order_forwarder_duplicate_formal_paste
// 07_higher_order_forwarder_duplicate_formal_paste
#define APPLY(F, G, X) F(G, X)
#define FWD_DUP(G, X) G(X, X)
#define JOIN(a, b) a##b

int APPLY(FWD_DUP, JOIN, aa) = 1;
