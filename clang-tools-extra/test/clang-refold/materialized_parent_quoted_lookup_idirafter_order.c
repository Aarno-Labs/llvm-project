// RUN: %clang-refold-tester-clang-flags materialized_parent_quoted_lookup_idirafter_order -- -idirafter %S/headers/real -I %S/headers/shadow
#include <parent.h>
int main(void) { return p + v + q; }
