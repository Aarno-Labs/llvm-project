// RUN: %clang-refold-tester tu_insertion_at_expansion_boundary_keeps_carrier
// Regression: a pure insertion landing at the gap where a macro expansion
// begins keeps its direct-TU carrier when it cannot be that macro's leading
// replacement-list literal.
//
// `SCALE(v)` expands to `((v) + 1)`, so its first expansion token is `(` -- and
// the TU token immediately before the expansion is also `(`, from `pick(`.
// Identical flanking spellings are what let the token LCS slide an inserted
// token across the seam, so the boundary guard treats this geometry as
// suspicious.  But possible is not actual: the guard exists because the
// inserted tokens might be the invocation's own leading `(` orphaned by that
// slide, and this insertion emits `ctx` `,` instead.  It cannot be the literal
// it is accused of supplying, so refusing it would surrender the whole
// translation unit to a verbatim copy for nothing.
//
// The opposite direction -- an insertion that really is the macro's own leading
// literal -- stays refused, and is covered by the seam regression added with
// the alignment-window-resolution work.
#define SCALE(v) ((v) + 1)

int ctx;

int pick(int a, int b) { return a + b; }

int chosen = pick(ctx, SCALE(2), 7);
