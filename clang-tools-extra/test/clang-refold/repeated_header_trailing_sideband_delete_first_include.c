// RUN: %clang-refold-tester-with-lines repeated_header_trailing_sideband_delete_first_include
#include "trailing_note.h"
int gap = 0;
#include "trailing_note.h"
int tail = 2;
