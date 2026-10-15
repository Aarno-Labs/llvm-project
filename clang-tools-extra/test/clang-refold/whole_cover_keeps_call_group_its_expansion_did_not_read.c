// RUN: %clang-refold-tester-verify-off whole_cover_keeps_call_group_its_expansion_did_not_read
// Regression: TU emission widened every patch whose replacement was not an
// identifier or a plain call over the `(...)` groups after its callsite,
// judged from the replacement's text.  AT's expansion reads nothing past
// `AT(0)`, so the whole cover realizing B's `tbl[0]` dropped the `(3)` that
// B keeps.  A whole cover now replaces exactly the source its expansion read,
// which the producer's use extents give, and emission never widens a patch.
typedef int (*fn)(int);
int id(int v) { return v; }
fn tab[2] = {id, id};
fn tbl[2] = {id, id};
#define AT(i) tab[i]
int r(void) { return AT(0)(3); }
