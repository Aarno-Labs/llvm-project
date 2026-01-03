// RUN: %clang-refold-tester macro_arg_edit_no_expansion2
#include "j.h"

int main() {
  int x = 5, y = 2;
  ADD_AND_RECORD("hello", x + y);
  return global_counter;
}
