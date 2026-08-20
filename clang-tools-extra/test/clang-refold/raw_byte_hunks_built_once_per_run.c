// RUN: %clang-refold-tester raw_byte_hunks_built_once_per_run
// RUN: FileCheck --input-file=%t/outputs/raw_byte_hunks_built_once_per_run.out %s
//
// Regression: the raw A/B byte diff must be built once per run, not once per
// pass.
//
// A replacement inside an object-like macro body concedes the macro root, which
// is evidence that demands alignment resolution.  Here `22` is deleted, so an A
// token of the certified window is unforced and the window does carry
// ambiguity -- the run cannot rule resolution out from the forced map alone and
// runs the resolution probe, which is a second trip through token-diff
// planning.  Resolution then commits nothing and the first pass stands.
//
// Token-diff planning ends by diffing the A and B source buffers at byte
// granularity.  Those buffers are constants of the run: an attempt narrows
// which owners must expand and which anchors it plans from, never the streams
// themselves.  So the probe -- and every narrowing attempt, and every candidate
// simulation -- presents the identical bytes and must replay the recorded hunks
// instead of re-deriving them.
//
// Rebuilding them is not a small waste.  The byte diff is Myers' algorithm over
// one element per source character, so its cost scales with the stream length
// times the A/B edit distance; on a corpus unit whose edit widens the stream
// several-fold it is the single largest cost in a pass, and this demand shape
// fires on essentially every such unit.
//
// The probe replays the hunks first; the attempt it stands in for reports the
// declined re-plan afterwards.
//
// CHECK: replaying this run's raw byte hunks
// CHECK: attempt 0 is limited by alignment ambiguity
#define SUM (11 + 22)
int v = SUM;
