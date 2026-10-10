// RUN: %clang-refold-tester-verify-off whole_cover_replaces_source_closing_argument_parentheses
// `CALL` leaves `f(H(a` open.  `G` pastes its argument, so `H` is never
// invoked, but the parenthesis after it is open too: source after the callsite
// closes it with the first `)` and `G`'s call with the second.  The
// realization must reach the last byte `G` consumed, not the first `)`: it used
// to emit `int y; int g_H(x)));` at verify-off.
#define G(v) g_##v
#define H(v) v
#define CALL(f, a) int a; int f(H(a
int y; int g_H(x);
