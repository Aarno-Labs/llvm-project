// RUN: %clang-refold-tester-with-lines boundary_insertion_multiple_sites_in_tu

// test82: insertions around boundary and later in TU.
#include "h82.h"
int INS82_BOUNDARY = hdr82_val + 1;
#line 5 "boundary_insertion_multiple_sites_in_tu.c"
int TU82_START = 82;
int INS82_LATE = TU82_START + 100;
#line 6 "boundary_insertion_multiple_sites_in_tu.c"
int TU82_NEXT = 182;
int main(){ return hdr82_val + TU82_START + TU82_NEXT; }
