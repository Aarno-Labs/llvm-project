
// RUN: %clang-refold-tester-with-lines mixed_owner_include_macro_include_partition LINES
// RUN: %clang-refold-tester mixed_owner_include_macro_include_partition NOLINES
#define ID(x) x

int value =
#include "partition_left.h"
  ID(2)
#include "partition_right.h"
;

int main(void) {
  printf("%s:%d\n", __FILE__, __LINE__);
  return value;
}
