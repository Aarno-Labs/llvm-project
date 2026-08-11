// RUN: %clang-refold-tester macro_boundary_literal_is_not_a_tu_insertion
// RUN: FileCheck --input-file=%t/outputs/macro_boundary_literal_is_not_a_tu_insertion.out %s
//
// Regression: the actual of `IN_USE` gains a leading `(`, so B has one more
// `(` than A in a run of identical parentheses spanning the seam between plain
// TU text and the macro's replacement list.
//
// The token LCS may anchor the `if`'s own `(` to the macro body's leading `(`
// and orphan the invocation's `(` into a pure zero-width insertion one token
// before the cover.  Realizing that insertion as direct TU text would supply a
// fixed replacement-list literal from outside the invocation, leaving the whole
// cover to realize an incomplete instance of `((x) >= 0)`.  The composed source
// then binds a different actual -- `IN_USE(ot->storage)` rather than
// `IN_USE((ot->storage)[j])` -- so `>= 0` tests the pointer instead of the
// element, and the result still compiles.
//
// The direct-TU carrier must refuse that seam.  With the unsound realization
// gone, the per-window semantic resolver is left with one admissible class and
// commits the alignment that keeps the edit inside the invocation.  Checking
// the emitted source alone would not distinguish this fix, because an
// unrelated defect also fails the refold, so assert the specific rejection too.
//
// CHECK: rejecting direct-TU insertion at A gap {{[0-9]+}}: B=[{{[0-9]+}},{{[0-9]+}}) would supply a replacement-list literal of macro {{[0-9]+}} (IN_USE) whose expansion cover is A=[{{[0-9]+}},{{[0-9]+}})
#define IN_USE(x)  ((x) >= 0)

struct S { int *storage; };

int probe(struct S *ot, int j)
{
  if (IN_USE((ot->storage)[j]))
    return 1;
  return 0;
}
