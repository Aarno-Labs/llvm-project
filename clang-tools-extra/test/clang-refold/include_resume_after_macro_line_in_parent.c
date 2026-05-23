// RUN: %clang-refold-tester-with-lines include_resume_after_macro_line_in_parent
#define LOGICAL_PARENT_LINE 500
#define LOGICAL_PARENT_FILE "logical_parent.c"
#line LOGICAL_PARENT_LINE LOGICAL_PARENT_FILE
#include "h_replace.h"
int observed = __LINE__;
