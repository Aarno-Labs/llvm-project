// RUN: %clang-refold-tester dag_paste_chain_numeric
#define CATP(a,b) a##b
#define CAT(a,b) CATP(a,b)
#define CAT3(a,b,c) CAT(CAT(a,b),c)
int v = CAT3(1,2,3);
