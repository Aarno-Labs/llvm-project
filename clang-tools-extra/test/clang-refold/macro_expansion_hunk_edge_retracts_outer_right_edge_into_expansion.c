// RUN: env CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_COST_BUDGET=0 %clang-refold-tester macro_expansion_hunk_edge_retracts_outer_right_edge_into_expansion
// Mirror of macro_expansion_hunk_edge_retracts_outer_left_edge_into_expansion:
// a hunk that begins inside a macro expansion and ends in plain
// translation-unit tokens retracts its right edge onto the expansion boundary.
//
// Rewriting `LOAD`'s argument `base + ob` to `next + (ot[0])` adds one `)` to
// the run that the new operand's closing paren shares with the parenthesized
// return expression.  Which B `)` the enclosing one matches is ambiguous, so
// the core-forced hunk `ob )` ends outside the expansion.  Retracting its left
// edge out of the expansion fails on `ob` against `(`, and widening stops at
// the `base` -> `next` hunk inside the same argument.  The enclosing `)` is the
// same on both sides, so handing it back leaves the hunk inside the expansion
// and the callsite replay keeps `LOAD`.
//
// Before, the ladder gave `LOAD` up and emitted its expansion in its place; the
// result verified only because the token outside the cover was identical on
// both sides.  The realization budget is zeroed so the core-forced plan has to
// stand on its own.
#define LOAD(p) *p

int probe(int *base, int ob, int *next, int *ot) {
  return (LOAD(base + ob));
}
