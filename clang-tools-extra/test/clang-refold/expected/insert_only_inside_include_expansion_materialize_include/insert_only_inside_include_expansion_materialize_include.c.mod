// RUN: %clang-refold-tester-with-lines insert_only_inside_include_expansion_materialize_include
#define KEEP(x) ((x) + 1)
int untouched = KEEP(5);
int values[] = { 1, 2, 3 };
