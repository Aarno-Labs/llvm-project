// RUN: %clang-refold-tester-with-lines header_prefix_insert_not_child_owned_by_sideband_replace
#line 1 "headers/h_parent_prefix_replace.h"
int before_child = 0;
#line 1 "headers/h_child_prefix_replace.h"
#pragma vendor beta
int child_value = 1;
#line 2 "headers/h_parent_prefix_replace.h"
int parent_tail = 9;
#line 3 "header_prefix_insert_not_child_owned_by_sideband_replace.c"
int tail = 3;
