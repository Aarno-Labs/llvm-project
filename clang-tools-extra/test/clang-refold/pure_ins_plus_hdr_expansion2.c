// RUN: %clang-refold-tester pure_ins_plus_hdr_expansion2 XXX
#include "d.h"

int main() {
  return FOO(5);
}
