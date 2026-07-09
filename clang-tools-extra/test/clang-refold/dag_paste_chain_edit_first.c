// RUN: %clang-refold-tester dag_paste_chain_edit_first
#define CATP(a,b) a##b
#define CAT(a,b) CATP(a,b)
#define CAT3(a,b,c) CAT(CAT(a,b),c)
int CAT3(x,y,z) = 1;
