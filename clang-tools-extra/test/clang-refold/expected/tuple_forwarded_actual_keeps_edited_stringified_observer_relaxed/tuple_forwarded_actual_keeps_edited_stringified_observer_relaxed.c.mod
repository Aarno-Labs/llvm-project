// RUN: %clang-refold-tester-relaxed-verify-off tuple_forwarded_actual_keeps_edited_stringified_observer_relaxed
// The relaxed pipeline tolerates a stringification B left stale, never one B
// edited, and that holds on the tuple-forwarded args-only path too.
//
// B renames both elements and edits `#PAIR` to "(x, w)", a literal that neither
// the old actual nor the rebuilt `(y, w)` stringifies to.  The slice validator
// never compared it, so this used to emit `CALL(ADD, (y, w))` at verify-off,
// which even the relaxed check rejects.
#define ADD(a, b) int a; int b;
#define CALL(F, PAIR) F PAIR char *s = #PAIR;
int y; int w; char *s = "(x, w)";
