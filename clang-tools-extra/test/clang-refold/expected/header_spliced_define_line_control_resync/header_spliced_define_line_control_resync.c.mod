// RUN: %clang-refold-tester-with-lines header_spliced_define_line_control_resync
int inserted = 0;
#include "h_spliced_define_line.h"
int tail = 3;
