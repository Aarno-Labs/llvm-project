// RUN: %clang-refold-tester-with-lines final_vaopt_multi_actual_deactivation
#define APPLY(F, G, X, ...) F(G, X, __VA_ARGS__)
#define FWD(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__))
#define FIELD_OPT_STR(name, ...) #name __VA_OPT__(":" #__VA_ARGS__)

const char *s = APPLY(FWD, FIELD_OPT_STR, alpha);
