// RUN: %clang-refold-tester-with-lines header_pragma_once_nested_reactivation
#line 1 "headers/outer2.h"
#define X_VALUE 42
int between = 0;
#line 3 "header_pragma_once_nested_reactivation.c"

int after = X_VALUE;
