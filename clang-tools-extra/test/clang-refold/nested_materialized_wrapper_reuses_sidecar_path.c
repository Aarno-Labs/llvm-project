// RUN: %clang-refold-tester-with-lines nested_materialized_wrapper_reuses_sidecar_path
// Source-graph alias bug: first top-level shared.h is dirty, and a later
// materialized wrapper leaves another #include "shared.h" in the final TU.
#define HDR_LINE 100
#define HDR_FILE "logical_shared.c"
#include "shared.h"
#include "wrapper2.h"
