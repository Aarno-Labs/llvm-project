// RUN: %clang-refold-tester-with-lines repeated_header_sideband_delete_first_include
#include "h_repeat_unknown.h"
#include "h_repeat_unknown.h"
int tail = 1;
