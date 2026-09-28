// RUN: %clang-refold-tester header_zero_token_include_gap_noncanonical_child_spelling
//
// Regression: a zero-token child include spelled with extra blanks and a
// trailing comment is preserved inside the rewritten header gap.  The producer
// synthesizes an include's `text` as `#include "name"`, so a check that the
// source bytes equal that text refused every other spelling; the recorded
// directive extent is the source spelling, whatever it is.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int before = 10,
#  include   "empty.inc"   /* contributes no tokens */
 after = 20;
