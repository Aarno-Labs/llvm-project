// RUN: %clang-refold-tester-with-lines pragma_once_later_include_reactivation
#include "x_with_pragma.h"

int between = 0;

#include "x_with_pragma.h"

int after = X_VALUE;
