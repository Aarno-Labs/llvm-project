// RUN: %clang-refold-tester-verify-off header_edit_seam_opening_call_realizes_include_from_b
// Regression: B drops the parentheses that kept the header's `(F)(g)` from
// calling the macro `F`.  The header edit planner read only the names its B
// text spells, and the deletions spell none, so the materialized header read
// `F(g)` under the live definition.  A header edit's B range now also reads
// the call its seam opens, and the include is realized from B, where the
// translation unit's partition repairs it.
int F(int v);
#define F(v) ((v) + 1)
int r = F(1);
#undef F
static int h(int g) { return F(g); }
int t = 0;
#define F(v) ((v) + 1)
int w = F(3);
