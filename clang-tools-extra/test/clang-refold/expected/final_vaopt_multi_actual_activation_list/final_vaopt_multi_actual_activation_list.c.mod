// RUN: %clang-refold-tester-with-lines final_vaopt_multi_actual_activation_list
#define APPLY(F, G, X, ...) F(G, X, __VA_ARGS__)
#define FWD(G, X, ...) G(X __VA_OPT__(, __VA_ARGS__))
#define LIST(name, ...) name __VA_OPT__(, __VA_ARGS__)

int arr[] = { APPLY(FWD, LIST, 1, 2, 3) };
