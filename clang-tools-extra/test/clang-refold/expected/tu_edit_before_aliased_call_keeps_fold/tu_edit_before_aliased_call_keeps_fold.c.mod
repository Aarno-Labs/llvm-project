// RUN: %clang-refold-tester-verify-off tu_edit_before_aliased_call_keeps_fold
// A TU byte edit between an object-like alias's #define and the call it forms
// is kept.
//
// `OBJ` expands to the name `F2`, which takes its `(x)` from the source after
// `OBJ`.  The producer records F2's [inv_b, inv_e) from that name in OBJ's
// #define to the `)`, so the range also covers `int z;`.  The TU byte-edit
// gate used to read that range as F2's callsite and refuse the edit of `z`,
// failing the whole refold.  The call's use is `OBJ(x)`, which the edit does
// not touch.
#define F2(a) int a; int g_##a;
#define OBJ F2
int w;
OBJ(x)
