// RUN: %clang-refold-tester-with-lines header_inactive_ifdef_define_line_control_resync
int inserted = 0;
#include "h_ifdef_line.h"
int tail = 3;
