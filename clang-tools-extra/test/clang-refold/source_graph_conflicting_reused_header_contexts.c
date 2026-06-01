// RUN: %clang-refold-tester-with-lines source_graph_conflicting_reused_header_contexts
// Source-graph bug: the same header path is included twice under different TU
// macro line-control state.  Both expansions are dirty, but the required owner
// bytes are different for each include site.
#define HDR_LINE 100
#define HDR_FILE "first_logical.c"
#include "shared.h"
#undef HDR_LINE
#undef HDR_FILE
#define HDR_LINE 200
#define HDR_FILE "second_logical.c"
#include "shared.h"
