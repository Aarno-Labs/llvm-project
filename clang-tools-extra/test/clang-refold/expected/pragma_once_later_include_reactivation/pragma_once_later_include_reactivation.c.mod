// RUN: %clang-refold-tester-with-lines pragma_once_later_include_reactivation
#define X_VALUE 42
int between = 0;
#line 7 "pragma_once_later_include_reactivation.c"

int after = X_VALUE;
