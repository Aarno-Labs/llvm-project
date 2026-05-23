// RUN: %clang-refold-tester-with-lines multi_hunk_inside_include_expansion_materialize_whole_include
#define KEEP(x) ((x) + 1)
int untouched = KEEP(5);
int values[] = { 10, 2, 30 };
