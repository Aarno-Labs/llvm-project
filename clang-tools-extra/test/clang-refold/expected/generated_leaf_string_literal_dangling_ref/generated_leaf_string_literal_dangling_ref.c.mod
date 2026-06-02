// RUN: %clang-refold-tester generated_leaf_string_literal_dangling_ref
#define APPLY(F, X, Y) F(X, Y)
#define JOIN(X, Y) X Y
const char *s = APPLY(JOIN, "alpha", "gamma");
