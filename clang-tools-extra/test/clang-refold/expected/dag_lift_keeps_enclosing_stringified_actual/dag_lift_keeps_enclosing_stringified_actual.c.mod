// RUN: %clang-refold-tester-verify-off dag_lift_keeps_enclosing_stringified_actual
// Lifting an edit into an enclosing invocation's actual must not leave that
// invocation's stringification of the actual stale under --strict.
//
// B edits `before(1)` but keeps `"ENTRY(before)"`, which `#x` spells from the
// actual text.  Lifting the edit to `OUTER(ENTRY(after))` would re-spell the
// literal, so the lift is refused and the invocation is expanded.  This used
// to emit `OUTER(ENTRY(after))` at verify-off.
static int before(int x) { return x; }
static int after(int x) { return x + 1; }
#define ENTRY(name) name(1)
#define OUTER(x) int v = x; const char *s = #x; (void)s; return v;
int main(void) { int v = after(1); const char *s = "ENTRY(before)"; (void)s; return v; }
