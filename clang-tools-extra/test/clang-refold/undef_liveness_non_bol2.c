// RUN: %clang-refold-tester undef_liveness_non_bol2
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#include "undef.h"
int use = FOO;
