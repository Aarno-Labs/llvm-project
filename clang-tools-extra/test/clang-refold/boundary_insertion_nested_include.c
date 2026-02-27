// RUN: %clang-refold-tester-with-lines boundary_insertion_nested_include

// test81: nested include boundary insertion.
#include "h81a.h"
int TU81_START = 81;
int main(){ return hdr81b_val + hdr81a_val + TU81_START; }
