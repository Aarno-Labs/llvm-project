// RUN: %clang-refold-tester-verify-off generated_leaf_unchanged_paste_observer_of_rewritten_formal
// A generated-leaf rewrite of a root actual must not leave another observer
// of that formal stale.
//
// B edits only the call `name(x)`; the pasted `f_##name` still spells
// `f_before`.  Rewriting the actual to `after` would re-expand the paste to
// `f_after`, so no args-only fold realizes B and the invocation is expanded.
// This used to emit `ENTRY(after)` at verify-off.
static int before(int x) { return x; }
static int after(int x) { return x + 1; }
#define ENTRY(name) static int f_##name(int x) { return name(x); }
static int f_before(int x) { return after(x); }
int main(void) { return f_before(1) != 2; }
