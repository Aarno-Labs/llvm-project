// RUN: %clang-refold-tester-with-lines generated_join_empty_second_activation
#define APPLY(F, G, A, B) F(G, A, B)
#define FWD_JOIN(G, A, B) G(A##B)
#define STR(x) #x

const char *s = APPLY(FWD_JOIN, STR, foo, bar);
