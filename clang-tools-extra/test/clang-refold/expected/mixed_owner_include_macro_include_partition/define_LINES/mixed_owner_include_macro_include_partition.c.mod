#line 1 "mixed_owner_include_macro_include_partition.c"

// RUN: %clang-refold-tester-with-lines mixed_owner_include_macro_include_partition LINES
// RUN: %clang-refold-tester mixed_owner_include_macro_include_partition NOLINES
#define ID(x) x

int value =
#line 1 "headers/partition_left.h"
10 +
#line 8 "mixed_owner_include_macro_include_partition.c"
  ID(20)
#line 1 "headers/partition_right.h"
+ 30
#line 10 "mixed_owner_include_macro_include_partition.c"
;

int main(void) {
  printf("%s:%d\n", __FILE__, __LINE__);
  return value;
}
