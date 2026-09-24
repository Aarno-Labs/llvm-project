// RUN: %clang-refold-tester include_hunk_edge_retracts_whole_trailing_include
// A hunk that ends with a whole included instance must hand the instance back
// when its tokens are unchanged, so the `#include` line survives.
//
// The edit writes `7` at the front of the initializer.  The header's one token
// is spelled like the `1` the edit kept, and deleting it is free because the
// gap after it belongs to this file, so no optimal alignment forces it.  The
// core-forced plan replaced `0, 1, <1>` with `7, 0, 1`: the include could not
// realize the file's tokens, the file had no spelling for the header's token,
// and the whole translation unit had no admissible refold.
//
// The instance's token is the same lexeme as B's last one, so handing it back
// keeps the edit exact and leaves a hunk this file owns.
#define IS_BLANK(c) ((c) == ' ')
int seven[] = { 7, 0,
#include "included_initializer_value.h"
};
int use(void) { return IS_BLANK(seven); }
