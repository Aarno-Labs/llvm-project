// RUN: %clang-refold-tester-with-lines higher_order_forwarder_context_stringify
#define APPLY(F, G, X) F(G, X)
#define FWD_SUFFIX(G, X) G(X) "!"
#define STR(x) #x

const char *s = APPLY(FWD_SUFFIX, STR, alpha);
