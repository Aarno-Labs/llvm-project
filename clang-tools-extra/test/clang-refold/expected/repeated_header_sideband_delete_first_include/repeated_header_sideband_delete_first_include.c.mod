// RUN: %clang-refold-tester-with-lines repeated_header_sideband_delete_first_include
int repeated = 1;
#include "h_repeat_unknown.h"
int tail = 2;
