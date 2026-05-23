// RUN: %clang-refold-tester-with-lines pragma_once_later_include_reactivation
#define X_VALUE 42
int between = 0;

int after = X_VALUE;
