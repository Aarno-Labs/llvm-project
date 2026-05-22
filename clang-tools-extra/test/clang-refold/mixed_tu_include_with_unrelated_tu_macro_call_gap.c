// RUN: %clang-refold-tester-with-lines mixed_tu_include_with_unrelated_tu_macro_call_gap
#define ID(x) x
int prefix = 0;
#include "value_a.h"
int gap = ID(7);
#include "value_b.h"
int suffix = 0;
