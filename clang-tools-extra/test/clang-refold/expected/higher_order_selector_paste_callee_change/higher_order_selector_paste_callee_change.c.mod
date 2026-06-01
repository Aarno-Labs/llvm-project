// RUN: %clang-refold-tester-with-lines higher_order_selector_paste_callee_change
#define APPLY(F, G, X) F(G, X)
#define FWD(G, X) G(X)
#define MAKE(x) pre_##x##_suf
#define MAKE2(x) post_##x##_end

int APPLY(FWD, MAKE2, alpha) = 1;
