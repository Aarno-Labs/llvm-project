// RUN: %clang-refold-tester-clang-flags materialized_parent_quoted_lookup_isystem_order -- -isystem %S/headers/real -I %S/headers/shadow
#include <parent.h>
int main(void) { return p + v + q; }
