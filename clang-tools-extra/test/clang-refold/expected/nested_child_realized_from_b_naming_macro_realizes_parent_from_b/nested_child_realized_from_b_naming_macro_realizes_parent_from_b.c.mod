// RUN: %clang-refold-tester-verify-off nested_child_realized_from_b_naming_macro_realizes_parent_from_b
// Regression: B renames the child header's `long` to the painted `M`, so the
// child is realized from B, inside a parent materialized from source for its
// own edit.  The parent's body is header source, which no translation-unit
// certificate reads, and the child's `M` expanded under the live definition.
// A child realized from B now gets the header path's reads before it is
// spliced into a source body, and the parent is realized from B instead.
typedef long M;
#define M ((M)0)
long m0 = M;
#undef M
int p0 = 5;
M c0 = 1;
int p1 = 2;
int t = 0;
#define M ((M)0)
long w = M;
