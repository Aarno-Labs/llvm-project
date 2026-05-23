// RUN: %clang-refold-tester-with-lines repeated_header_sideband_only_insert_second
#include "pragma_only2.h"
int gap = 0;
#include "pragma_only2.h"
int value = 1;
