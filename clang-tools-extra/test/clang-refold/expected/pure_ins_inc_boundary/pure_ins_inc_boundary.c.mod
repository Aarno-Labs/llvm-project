// RUN: %clang-refold-tester pure_ins_inc_boundary
#include "a.h"
void aaa(int x);
#include "b.h"
int zzz(char* c);
#include "c.h"

int main() {
  return 0;
}
