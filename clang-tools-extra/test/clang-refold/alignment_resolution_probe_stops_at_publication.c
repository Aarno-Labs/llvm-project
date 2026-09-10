// RUN: %clang-refold-tester alignment_resolution_probe_stops_at_publication
// RUN: FileCheck --input-file=%t/outputs/alignment_resolution_probe_stops_at_publication.out %s
//
// Regression: the alignment-resolution probe must stop at the point where
// resolution publishes its theorem, not at the end of token-diff planning.
//
// A replacement inside an object-like macro body concedes the macro root, and
// the certified window carries an unforced A token, so the run cannot rule
// resolution out from the forced map alone.  It therefore runs the probe: a
// second trip through token-diff planning whose entire output is the recorded
// resolution memo.
//
// That memo is written from inside the token-diff planner's Plan() call.
// Everything token-diff planning does afterwards -- the witness-ledger audit,
// hunk-edge retraction out of partially owned expansions, mixed-owner
// structural tiling, insertion-provenance construction -- builds the plan for
// an emission the probe never performs, and on a stream-widening edit it is
// the overwhelming majority of the probe's cost.
//
// `refold model loaded` is logged once per completed token-diff plan.  Two
// passes start here and only one plan completes, which is exactly the claim:
// the probe returned at the publication point.  Each pass names its own role,
// so the second start is asserted to be the probe rather than inferred from
// its position.
//
// CHECK: starting refold [production attempt 0]:
// CHECK: refold model loaded:
// CHECK: attempt 0 is limited by alignment ambiguity, and {{[0-9]+}} of {{[0-9]+}} certified window(s) carry it
// CHECK: starting refold [alignment resolution probe]:
// CHECK-NOT: refold model loaded:
// CHECK: alignment resolution probe finished:
// CHECK: attempt 0 is limited by alignment ambiguity, but
#define SPAN (7 + 13)
int width = SPAN;
