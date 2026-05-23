// RUN: %clang-refold-tester-with-lines repeated_header_sideband_replace_second_include
#include "replace_note.h"
int gap = 0;
#pragma vendor replace_note_changed
int z = 1;
int tail = 2;
