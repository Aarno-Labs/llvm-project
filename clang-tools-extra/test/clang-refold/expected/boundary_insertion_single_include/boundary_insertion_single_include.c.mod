// RUN: %clang-refold-tester-with-lines boundary_insertion_single_include

// test80: include boundary insertion (single include).
#include "h80.h"
int INS80_BOUNDARY = hdr80_val + 1;
int TU80_START = 80;
int main(){ return hdr80_val + TU80_START; }
