// RUN: %clang-refold-tester whole_cover_replays_pragma_operator_in_consumed_source
// A whole cover may replace a `_Pragma` operator in the source its expansion
// consumed when it prints that operator's pragma in its place.
// `CALL(G, x)` leaves `G(x` open, and the `_Pragma` and the second `)` after
// it are read as the rest of G's actual, so the pragma is printed after
// `g_x`.  B renames CALL's first `x` but keeps `g_x`, which only an expansion
// realizes, and the expansion replaces those consumed bytes too.  The
// operator prints nothing but its `#pragma` line and changes no state, so
// replaying B's copy of that line reproduces it, and the edit is licensed to
// remove exactly that operator.  This used to find no admissible refold.
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
CALL(G, x) _Pragma("refold_marker") );
