// RUN: %clang-refold-tester-with-lines repeated_header_sideband_replace_second_include
#include "replace_note.h"
int gap = 0;
#include "replace_note.h"
int tail = 2;
