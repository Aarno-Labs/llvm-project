// RUN: %clang-refold-tester macro_arg_insertion
#include "j.h"

int main() {
  int x = 5, y = 2;
  ADD_AND_RECORD("hello", x + y);
  return global_counter;
}
