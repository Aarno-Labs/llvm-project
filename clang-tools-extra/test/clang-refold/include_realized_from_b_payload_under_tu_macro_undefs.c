// RUN: %clang-refold-tester-verify-off include_realized_from_b_payload_under_tu_macro_undefs
// An include realized from B lands under the translation unit's macro state at
// the `#include`, so a payload naming a live TU macro is repaired there too.
//
// B inserts `int FROM_B_V;` inside the header.  A header edit may not write a
// B payload naming a recorded macro, so the include is realized from B.  That
// edit is staged only at final emission, after the planning pass repaired and
// audited liveness, and neither used to see it: the output kept
// `#define FROM_B_V 3` live over the slice and emitted `int 3;`.  Both now run
// again after the include edits are staged.
#define FROM_B_V 3
int from_b_a = FROM_B_V;
#include "include_realized_from_b_under_tu_macro.h"
