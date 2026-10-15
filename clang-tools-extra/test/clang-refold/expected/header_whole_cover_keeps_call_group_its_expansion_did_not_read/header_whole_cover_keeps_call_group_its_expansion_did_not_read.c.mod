// RUN: %clang-refold-tester-verify-off header_whole_cover_keeps_call_group_its_expansion_did_not_read
// Regression: the header materializer widened a header patch over the `(...)`
// groups after its callsite by the same text test as TU emission, so the
// whole cover realizing B's `tbl[0]` dropped the `(3)` that B keeps.  A header
// patch now replaces exactly the range it was built over.
typedef int (*fn)(int);
int id(int v) { return v; }
fn tab[2] = {id, id};
fn tbl[2] = {id, id};
#define AT(i) tab[i]
int r(void) { return tbl[0](3); }
int w = 3;
