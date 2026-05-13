// RUN: %clang-refold-tester header_macro_state_liveness_child_include_undef
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

#define FOO 7
int before = 10,
#include "undef_foo.h"
 after = 20;
int use = FOO;
