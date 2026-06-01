// RUN: %clang-refold-tester-with-lines tuple_callee_token_paste_rewrite
#define CALL2(F, A, B) F(A, B)
#define WRAP(PAIR) CALL2 PAIR
#define CAT(a, b) a##b

int value = WRAP((CAT, a, b));
