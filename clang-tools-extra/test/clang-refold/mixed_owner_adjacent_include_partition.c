// RUN: %clang-refold-tester-with-lines mixed_owner_adjacent_include_partition LINES
// RUN: %clang-refold-tester mixed_owner_adjacent_include_partition NOLINES
int values[] = {
#include "x.h"
#include "y.h"
#include "z.h"
};

int main(void) {
  printf("%s:%d\n", __FILE__, __LINE__);
  return values[0];
}
