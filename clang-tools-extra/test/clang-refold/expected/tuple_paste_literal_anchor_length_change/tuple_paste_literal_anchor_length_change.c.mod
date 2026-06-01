// RUN: %clang-refold-tester-with-lines tuple_paste_literal_anchor_length_change
#define JOIN(a, b) a ## _ ## b
#define CALL(F, A, B) F(A, B)
#define WRAP(PAIR) CALL PAIR

int WRAP((JOIN, alpha, beta)) = 1;
