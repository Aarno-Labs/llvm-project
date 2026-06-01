// RUN: %clang-refold-tester-with-lines header_line_operands_from_tu_macro_state
#define HDR_LINE 800
#define HDR_FILE "hdr_from_tu.c"
#include "correct_header.h"
