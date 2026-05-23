// RUN: %clang-refold-tester-with-lines literal_line_value_edit_no_location_proof
int pre0 = 0;
int pre1 = 1;
int value = __LINE__;
int tail = 3;
