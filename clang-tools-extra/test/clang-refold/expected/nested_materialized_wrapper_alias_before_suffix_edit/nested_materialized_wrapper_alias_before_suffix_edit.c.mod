// RUN: %clang-refold-tester-with-lines nested_materialized_wrapper_alias_before_suffix_edit
// Source-graph alias bug variant: the nested shared.h include appears before
// the wrapper-owned edited suffix.
#define HDR_LINE 100
#define HDR_FILE "logical_shared.c"
int inserted_first = 1;
#line HDR_LINE HDR_FILE
int value = 101;
const char *file = __FILE__;
#include "wrapper.h"
int wrapper_insert_after = 2;
