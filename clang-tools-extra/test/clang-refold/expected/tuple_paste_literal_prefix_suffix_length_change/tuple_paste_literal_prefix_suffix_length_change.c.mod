// RUN: %clang-refold-tester-with-lines tuple_paste_literal_prefix_suffix_length_change
#define MAKE(name) pre_ ## name ## _suf
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR

int WRAP((MAKE, longer)) = 1;
