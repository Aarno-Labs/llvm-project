// RUN: %clang-refold-tester macro_arg_insertion
#include "j.h"

int main() {
  int x = 5, y = 2;
  do { long tmp_val = (x + y * 2); ++global_counter; } while (0);
  return global_counter;
}
