// RUN: %clang-refold-tester-with-lines insert_only_inside_include_expansion_materialize_include
#define KEEP(x) ((x) + 1)
int untouched = KEEP(5);
#line 1 "headers/array_values.h"
int values[] = { 1, 2, 3 };
#line 5 "insert_only_inside_include_expansion_materialize_include.c"
