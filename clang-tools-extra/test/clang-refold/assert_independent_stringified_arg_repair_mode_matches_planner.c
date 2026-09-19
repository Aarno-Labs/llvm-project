// RUN: %clang-refold-tester-relaxed-verify-repair assert_independent_stringified_arg_repair_mode_matches_planner
//
// The `--verify-output=repair` counterpart of
// assert_independent_stringified_arg_relaxed_planner_expands, on the same A/B.
// It pins that repair mode does not perturb a result the planner already
// proved: both emit the same source, so the mode is not silently buying the
// answer.
//
// It used to pin more than that.  Before the relaxed stringify tolerance
// required staleness, the planner admitted the unsound fold
// `assert(x == z || x == y)` here, the closing verification rejected the
// assembly, and the ladder expanded the owning region until the emitted source
// replayed the edited stream.  The name said so: `audit_repair_expands`.  The
// planner now refuses that fold on its own evidence and lands on whole cover
// directly, so no divergence is raised and nothing is expanded.  The test kept
// passing with its claim no longer true, which is why it was renamed rather
// than left alone.
//
// COVERAGE GAP, recorded here because this test is where a reader will look for
// it: what distinguishes `repair` from `fatal` is the disposition on a Fail-kind
// verdict -- and on an inconclusive one, where the refolded source cannot be
// preprocessed at all.  No test exercises either branch.  The narrowing ladder
// itself stays well covered (166 tests in this suite log `verified after 1
// narrowing step(s)`, under `fatal`), so what is uncovered is the disposition,
// not the machinery.  Closing the gap needs a shape whose assembly genuinely
// diverges under the relaxed pipeline; none is known, and one should be fixed
// rather than pinned if it is found.
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
assert(x == y);
