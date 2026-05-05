#line 1 "mixed_owner_adjacent_include_partition.c"
// RUN: %clang-refold-tester-with-lines mixed_owner_adjacent_include_partition LINES
// RUN: %clang-refold-tester mixed_owner_adjacent_include_partition NOLINES
int values[] = {
10,
#line 6 "mixed_owner_adjacent_include_partition.c"
#include "z.h"
};

int main(void) {
  printf("%s:%d\n", __FILE__, __LINE__);
  return values[0];
}
