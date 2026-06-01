// RUN: %clang-refold-tester-with-lines nested_materialized_wrapper_alias_before_suffix_edit
// Source-graph alias bug variant: the nested shared.h include appears before
// the wrapper-owned edited suffix.
#define HDR_LINE 100
#define HDR_FILE "logical_shared.c"
#include "shared.h"
#include "wrapper.h"
