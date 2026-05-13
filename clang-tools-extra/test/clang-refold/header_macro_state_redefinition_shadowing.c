// RUN: %clang-refold-tester header_macro_state_redefinition_shadowing
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#include "redef_shadow.h"
