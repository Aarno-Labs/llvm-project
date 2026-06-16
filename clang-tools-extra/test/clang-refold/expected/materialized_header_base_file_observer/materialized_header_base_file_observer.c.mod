// RUN: %clang-refold-tester-clang-flags materialized_header_base_file_observer -- -I %S
int p = 3;
const char *v = "materialized_header_base_file_observer.c";
int q = 2;
int main(void) { return p + q; }
