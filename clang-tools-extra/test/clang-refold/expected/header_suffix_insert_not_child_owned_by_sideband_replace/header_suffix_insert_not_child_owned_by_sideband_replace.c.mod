// RUN: %clang-refold-tester-with-lines header_suffix_insert_not_child_owned_by_sideband_replace
#line 1 "headers/h_parent_suffix_replace.h"
#line 1 "headers/h_child_suffix_replace.h"
#pragma vendor beta
int child_value = 1;
#line 2 "headers/h_parent_suffix_replace.h"
int after_child = 0;
int parent_tail = 9;
#line 3 "header_suffix_insert_not_child_owned_by_sideband_replace.c"
int tail = 3;
