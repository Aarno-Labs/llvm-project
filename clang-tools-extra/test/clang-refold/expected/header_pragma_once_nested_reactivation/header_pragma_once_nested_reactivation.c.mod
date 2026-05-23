// RUN: %clang-refold-tester-with-lines header_pragma_once_nested_reactivation
#define X_VALUE 42
int between = 0;

int after = X_VALUE;
