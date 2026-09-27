// RUN: env CLANG_REFOLD_TEST_ONLY_FORCE_EXPAND_INCLUDE=forced_expand_marked.h %clang-refold-tester alignment_resolution_replay_skips_attempt_that_gave_up_an_owner
// RUN: FileCheck --input-file=%t/outputs/alignment_resolution_replay_skips_attempt_that_gave_up_an_owner.out %s
//
// Regression: the alignment resolution replay compares production only
// against a simulation planned under the same inputs.
//
// Candidate simulations plan with no owner given up; that is what makes
// resolution one answer per run.  The test-only hook gives up the include
// from the first attempt, so production inlines the header while every
// simulation kept the `#include`.  The outputs legitimately differ, and
// comparing them would refuse a sound refold.  The replay is skipped instead,
// and the declaration of `x` is still deleted by the structure-respecting
// repair, as in
// alignment_window_commits_structure_respecting_repair_when_decline_fails.c.
//
// CHECK: window 0 committed StructureRespectingTerminalRecovery
// CHECK: alignment resolution replay not checked: this attempt gave up 1 owner(s) that witness 1's simulation planned without
// CHECK-NOT: did not reproduce
#include "forced_expand_marked.h"
int y;
#define A
static int
x;
#define B
static int z;
