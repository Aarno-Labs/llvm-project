// RUN: %clang-refold-tester pure_ins_plus_hdr_expansion1 XXX
#include "d.h"

int main() {
  return FOO(5);
}
