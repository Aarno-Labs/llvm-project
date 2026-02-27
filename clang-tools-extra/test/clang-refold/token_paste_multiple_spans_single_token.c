// RUN: %clang-refold-tester-with-lines token_paste_multiple_spans_single_token

// test72: Multiple paste spans in one token.
#define M2(A,B) int A##__##B = 7;
M2(left,right)
int main(){ return left__right; }
