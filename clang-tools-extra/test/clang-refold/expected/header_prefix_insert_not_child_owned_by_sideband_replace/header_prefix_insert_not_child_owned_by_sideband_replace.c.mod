// RUN: %clang-refold-tester-with-lines header_prefix_insert_not_child_owned_by_sideband_replace
int before_child = 0;
#pragma vendor beta
int child_value = 1;
int parent_tail = 9;
int tail = 3;
