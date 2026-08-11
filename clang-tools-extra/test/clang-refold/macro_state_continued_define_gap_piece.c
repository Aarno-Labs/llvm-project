// RUN: %clang-refold-tester macro_state_continued_define_gap_piece

// A backslash-continued #define inside a materialized header's preserved gap.
// The producer records directive text by rendering it from parsed MacroInfo
// tokens, so its canonical single-space spelling never equals the tab- and
// continuation-bearing source bytes.  Recovering the directive's source interval
// from that text therefore returned nothing, no gap piece was built for it, and
// the surrounding source envelope lost its authorized owner.  The producer now
// records the directive's physical extent, so the whole logical line survives.
int untouched = 1;

#include "continued_define_gap.h"
