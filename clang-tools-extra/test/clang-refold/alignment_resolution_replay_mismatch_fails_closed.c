// RUN: %clang-refold-tester alignment_resolution_replay_mismatch_fails_closed
// RUN: FileCheck --input-file=%t/outputs/alignment_resolution_replay_mismatch_fails_closed.out %s
// RUN: env CLANG_REFOLD_TEST_ONLY_PERTURB_ALIGNMENT_REPLAY_REFERENCE=1 %clang-refold-tester-expect-refold-fail alignment_resolution_replay_mismatch_fails_closed
// RUN: FileCheck --check-prefix=PERTURBED --input-file=%t/outputs/alignment_resolution_replay_mismatch_fails_closed.out %s
//
// Regression: production must reproduce the concrete output that the
// simulation of its committed alignment resolution produced, or fail closed.
//
// The shape is the structure-respecting repair's: deleting the two-line
// declaration of `x` leaves four optimal alignments, declining requests
// terminal fallback, and the one map straddling no `#define` is committed.
// Production then plans that map with the window's structural tiling ties
// settled for preserved structure, exactly as its simulation did, and before
// final line-control pruning its output must equal the simulation's.
//
// The test-only hook compares production against a reference no output can
// equal.  The mismatch requests terminal fallback, which no narrowing can
// repair, so the refold refuses rather than emitting unchecked source.
//
// CHECK: window 0 committing structure-respecting repair
// CHECK: alignment resolution reproduced: production's concrete output equals witness 1's simulation
// CHECK-NOT: did not reproduce
//
// PERTURBED: window 0 committing structure-respecting repair
// PERTURBED: alignment resolution did not reproduce: production's concrete output differs from witness 1's simulation; taking the terminal fallback
// PERTURBED: stage=alignment-resolution-replay
int y;
#define A
static int
x;
#define B
static int z;
