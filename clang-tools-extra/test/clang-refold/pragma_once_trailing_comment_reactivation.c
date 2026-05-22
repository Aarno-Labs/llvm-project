// RUN: %clang-refold-tester-with-lines pragma_once_trailing_comment_reactivation
#include "x2.h"

int between = 0;

#include "x2.h"

int after = X_VALUE;
