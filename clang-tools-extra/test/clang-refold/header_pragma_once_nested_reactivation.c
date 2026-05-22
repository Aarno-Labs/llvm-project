// RUN: %clang-refold-tester-with-lines header_pragma_once_nested_reactivation
#include "outer2.h"

int after = X_VALUE;
