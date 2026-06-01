// RUN: %clang-refold-tester-with-lines source_graph_dirty_first_reused_header
// Source-graph bug: the first include is dirty, the second include of the same
// header is untouched.  A sidecar written to the original relative include path
// changes both include sites, so preserving the include edge is unsound here.
#define HDR_LINE 100
#define HDR_FILE "logical_shared.c"
int inserted = 1;
#line HDR_LINE HDR_FILE
int value = 101;
const char *file = __FILE__;
#include "shared.h"
