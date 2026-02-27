// RUN: %clang-refold-tester-with-lines boundary_insertion_multiple_sites_in_tu

// test82: insertions around boundary and later in TU.
#include "h82.h"
int TU82_START = 82;
int TU82_NEXT = 182;
int main(){ return hdr82_val + TU82_START + TU82_NEXT; }
