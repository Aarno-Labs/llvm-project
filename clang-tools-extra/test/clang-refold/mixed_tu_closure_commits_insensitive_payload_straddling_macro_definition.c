// RUN: %clang-refold-tester mixed_tu_closure_commits_insensitive_payload_straddling_macro_definition
// A single B token replacing material on both sides of a `#define` must still
// preserve the directive, in place.
//
// Same shape as the consumed-pragma case, one directive family further on. The
// structural tiler splits a replacement hunk at the preserved directive by
// projecting the A seam onto one exact B boundary; the payload is one token, so
// the projection returns a range and the partition is not unique.  A `#define`
// is consumed exactly as `GCC poison` is -- it contributes no token to either
// stream, so moving the payload across it changes no token order -- and the
// state it changes is the definition bound to one macro name, which a payload
// naming no identifier cannot reach.
//
// Reaching that conclusion is not enough on its own.  A macro-state separator
// is reserved for the macro-state liveness planner at two further gates, and
// that reservation is correct wherever the name still matters downstream.  Here
// nothing observes `VALUE` after the directive, so the planner has no ordering
// to decide and the seam is the tiler's to split.  The directive keeps its
// line; the payload is committed after it.
int arr[] = { 1,
#define VALUE 3
2 };
