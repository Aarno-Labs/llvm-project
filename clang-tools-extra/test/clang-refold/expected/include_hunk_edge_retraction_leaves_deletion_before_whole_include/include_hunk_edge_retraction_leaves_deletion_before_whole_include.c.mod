// RUN: %clang-refold-tester include_hunk_edge_retraction_leaves_deletion_before_whole_include
// Deleting the initializer value in front of an included one must keep the
// `#include` line.
//
// The header's one token is spelled like the `1` the edit deletes, and
// deleting it is free because the gap after it belongs to this file, so
// neither `1` is forced.  The core-forced plan replaced `1, <1>` with `1`.
// Retracting its right edge off the whole instance consumes the whole B side,
// which the retraction used to refuse; the include was then given up.  The
// retracted hunk is a deletion this file owns.
int seven[] = { 
#include "included_initializer_value.h"
};
int use(void) { return seven[0]; }
