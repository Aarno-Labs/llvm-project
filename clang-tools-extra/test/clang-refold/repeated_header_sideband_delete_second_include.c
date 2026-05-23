// RUN: %clang-refold-tester-with-lines repeated_header_sideband_delete_second_include
#include "repeated_note.h"
int gap = 0;
#include "repeated_note.h"
int tail = 2;
