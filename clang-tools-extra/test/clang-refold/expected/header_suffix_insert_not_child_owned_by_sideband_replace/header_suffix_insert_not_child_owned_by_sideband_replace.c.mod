// RUN: %clang-refold-tester-with-lines header_suffix_insert_not_child_owned_by_sideband_replace
#pragma vendor beta
int child_value = 1;
int after_child = 0;
int parent_tail = 9;
int tail = 3;
