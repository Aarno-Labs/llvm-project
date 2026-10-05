// RUN: %clang-refold-tester-verify-off dag_lift_keeps_descendant_stringified_expansion
// Lifting an edit into an enclosing invocation's actual must not leave a
// generated invocation's stringification of that actual stale under --strict.
//
// `OUTER` forwards its actual to `S(x)`, which stringifies the expansion
// `before(1)`.  B edits the direct use but keeps `"before(1)"`; lifting the
// edit to `OUTER(ENTRY(after))` would re-spell it, so the lift is refused and
// the invocation is expanded.  This used to emit `OUTER(ENTRY(after))` at
// verify-off.
static int before(int x) { return x; }
static int after(int x) { return x + 1; }
#define ENTRY(name) name(1)
#define S(y) #y
#define OUTER(x) int v = x; const char *s = S(x); (void)s; return v;
int main(void) { OUTER(ENTRY(before)) }
