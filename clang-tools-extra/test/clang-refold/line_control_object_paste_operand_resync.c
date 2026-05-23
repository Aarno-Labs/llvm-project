// RUN: %clang-refold-tester-with-lines line_control_object_paste_operand_resync
#define A 7
#define A00 700
#define LOC A ## 00 "logical_object_paste.c"
#line LOC
int before = __LINE__;
int keep = __LINE__;
