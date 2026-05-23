// RUN: %clang-refold-tester-with-lines repeated_header_sideband_only_insert_second
#include "pragma_only2.h"
int gap = 0;
#pragma vendor alpha
#pragma vendor beta
int value = 2;
