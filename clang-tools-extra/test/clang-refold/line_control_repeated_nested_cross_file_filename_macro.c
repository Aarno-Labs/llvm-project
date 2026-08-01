// RUN: %clang-refold-tester-with-lines line_control_repeated_nested_cross_file_filename_macro
// Cross-file repeated nested line-control macro.  PAIR's replacement 'F x' has
// a body invocation of F (spelled in the header) and an arg F (spelled in the
// TU), so the two same-spelled F children of PAIR straddle two files -- the
// unsound shape for a positional source-order map.  It is refolded
// structure-preserving ONLY because both F expand identically ("gap.c"), so the
// occurrence assignment is order-independent; a differing-expansion sibling set
// would instead fail closed.  See findLineControlMacroOccurrenceForChild.
#include "b9pair.h"
int x =
1 +
#line 123 PAIR(F)
#include "two.inc"
;
int y = __LINE__;
const char *f = __FILE__;
