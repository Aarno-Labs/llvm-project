// RUN: %clang-refold-tester preserved_define_gap_piece_keeps_source_spelling

// A #define preserved as a gap piece must be re-emitted as its source bytes.
// MacroDirective::text is rendered from the parsed MacroInfo, so it drops the
// trailing comment, collapses the tabs, and adds a trailing space for the empty
// replacement list.  Emitting that rendering rewrites the directive and, by
// folding physical lines away, moves every line observer in the header suffix.
int untouched = 1;

#include "zero_token_define_trailing_comment.h"
