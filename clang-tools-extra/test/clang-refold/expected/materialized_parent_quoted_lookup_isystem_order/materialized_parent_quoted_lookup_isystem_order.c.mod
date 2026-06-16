// RUN: %clang-refold-tester-clang-flags materialized_parent_quoted_lookup_isystem_order -- -isystem %S/headers/real -I %S/headers/shadow
int p = 3;
int v = 10;
int q = 2;
int main(void) { return p + v + q; }
