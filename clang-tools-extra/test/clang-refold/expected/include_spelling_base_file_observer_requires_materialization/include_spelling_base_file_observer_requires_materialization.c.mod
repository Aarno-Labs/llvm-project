// RUN: %clang-refold-tester-clang-flags include_spelling_base_file_observer_requires_materialization -- -I headers/include_spelling_base
int p = 3;
const char *base = "include_spelling_base_file_observer_requires_materialization.c";
int q = 2;
int main(void) { return p + q + (base[0] != 0); }
