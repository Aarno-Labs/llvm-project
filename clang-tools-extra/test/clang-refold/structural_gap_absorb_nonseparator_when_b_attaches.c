// RUN: %clang-refold-tester structural_gap_absorb_nonseparator_when_b_attaches
// General leftward-gap absorption driven by B's own spacing, not a punctuation
// whitelist.  The inserted token is '*', a NON-separator that the old
// comma/semi/colon whitelist would have refused.  Because B places it directly
// against ')' (no whitespace before it in B), the rewrite absorbs the source
// gap and reproduces B's adjacency: "(a) (b)" -> "(a)*(b)".  The complementary
// keep-the-space direction (B spaces a non-separator, e.g. "= y") is covered by
// macro_boundary_ins.c.
int c = (a) (b);
