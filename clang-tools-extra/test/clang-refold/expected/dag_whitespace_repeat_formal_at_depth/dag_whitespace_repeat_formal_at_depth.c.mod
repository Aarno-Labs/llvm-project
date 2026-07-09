// RUN: %clang-refold-tester dag_whitespace_repeat_formal_at_depth
#define REP(x) x x
#define WRAPR(x) REP(x)
int a[]={WRAPR(9)};
