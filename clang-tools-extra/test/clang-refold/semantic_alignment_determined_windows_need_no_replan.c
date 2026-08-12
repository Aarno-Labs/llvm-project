// RUN: %clang-refold-tester semantic_alignment_determined_windows_need_no_replan
// RUN: FileCheck --input-file=%t/outputs/semantic_alignment_determined_windows_need_no_replan.out %s
//
// Regression: an insertion inside an object-like macro body concedes the macro
// root.  A conceded root is evidence that demands alignment resolution, so the
// run asks for it -- but there is nothing here for resolution to resolve.
//
// Every A token of every certified window is forced.  The inserted tokens match
// nothing in A, and the one repeated spelling, `+`, cannot move: mapping A's `+`
// to the inserted one would strand `22`, which costs a match and is therefore
// not optimal.  A window whose A tokens are all forced-matched admits exactly
// one optimal map, so resolution passes over every window and commits nothing.
//
// The run must reach that conclusion from the forced map it already retained.
// Planning a second pass to rediscover it costs a complete pipeline pass and
// reproduces the first pass byte for byte -- the shape that dominates large
// corpus units, where the demand evidence fires on essentially every unit.
//
// CHECK: the core theorem determined every certified window; resolution has nothing to commit
#define SUM (11 + 22)
int v = SUM;
