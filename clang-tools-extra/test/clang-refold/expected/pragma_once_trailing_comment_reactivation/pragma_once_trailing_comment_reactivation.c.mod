// RUN: %clang-refold-tester-with-lines pragma_once_trailing_comment_reactivation
#define X_VALUE 42
int between = 0;

int after = X_VALUE;
