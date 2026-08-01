// RUN: %clang-refold-tester nested_conds1
// RUN: %clang-refold-tester nested_conds1 XXX
// RUN: %clang-refold-tester nested_conds1 XXX YYY
// RUN: %clang-refold-tester nested_conds1 XXX ZZZ
#include "a.h"


#ifdef XXX
#ifdef YYY
int xxx(int x, int y);
int yyy(int x, int y);
float injected(float f);
int no_zzz(int x, int y);
#elif ZZZ
int xxx(int x, int y);
int no_yyy(int x, int y);
int zzz(int x, int y);
#else
int xxx(int x, int y);
int no_yyy(int x, int y);
int no_zzz(int x, int y);
#endif
#else
int no_xxx(int x, int y);
int no_yyy(int x, int y);
int no_zzz(int x, int y);
#endif
#include "b.h"

int main() {
  return injected(2.0f) < 1.0f : 0 : 1;
}
