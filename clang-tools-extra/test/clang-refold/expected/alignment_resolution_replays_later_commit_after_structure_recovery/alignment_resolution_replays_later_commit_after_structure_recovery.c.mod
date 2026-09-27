// RUN: env CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET=1000000 %clang-refold-tester alignment_resolution_replays_later_commit_after_structure_recovery
// RUN: FileCheck --input-file=%t/outputs/alignment_resolution_replays_later_commit_after_structure_recovery.out %s
//
// Regression: when a structure-respecting recovery is followed by a later
// committing window, production must reproduce the later window's
// simulation, the only one that realized the final selection.
//
// The budget splits this file into two certification windows.  Window 0
// deletes the declaration of `x` between two `#define`s, the shape of
// alignment_window_commits_structure_respecting_repair_when_decline_fails.c:
// declining requests terminal fallback, so the structure-respecting repair
// commits the one map straddling neither directive, and its window's
// structural tiling ties are settled for preserved structure.  Window 1
// rewrites an `EXPR3` invocation whose optimal maps all realize one output,
// so it commits a complete-ground-set equivalence.
//
// Window 1 was simulated with window 0's choice and tie range, which is the
// final selection, so the replay compares production against witness 2.
// Window 0 was simulated with window 1 still at its core-forced anchors.  In
// this file that realization happens to emit the same output, so this test
// pins which witness is replayed; RefoldAlignmentSemanticResolverTests pins
// that the earlier window's key can differ.
//
// The budget must also leave room for the repair's sub-rectangle enumeration,
// which reserves its worst case against the window's byte budget: below about
// 650000 bytes the repair cannot run, and above about 1400000 the file
// certifies as one window.
//
// CHECK: window 0 committed StructureRespectingTerminalRecovery
// CHECK: window 1 committed CompleteGroundSetEquivalence
// CHECK: committed semantic alignment resolution: kinds=[StructureRespectingTerminalRecovery,CompleteGroundSetEquivalence] witnesses=2
// CHECK: alignment resolution reproduced: production's concrete output equals witness 2's simulation
// CHECK-NOT: did not reproduce
int y;
#define A

#define B
static int z;
int lead_0 = 5000;
int lead_1 = 5001;
int lead_2 = 5002;
int lead_3 = 5003;
int lead_4 = 5004;
int lead_5 = 5005;
int lead_6 = 5006;
int lead_7 = 5007;
int lead_8 = 5008;
int lead_9 = 5009;
int lead_10 = 5010;
int lead_11 = 5011;
int lead_12 = 5012;
int lead_13 = 5013;
int lead_14 = 5014;
int lead_15 = 5015;
int lead_16 = 5016;
int lead_17 = 5017;
int lead_18 = 5018;
int lead_19 = 5019;
int lead_20 = 5020;
int lead_21 = 5021;
int lead_22 = 5022;
int lead_23 = 5023;
int lead_24 = 5024;
int lead_25 = 5025;
int lead_26 = 5026;
int lead_27 = 5027;
int lead_28 = 5028;
int lead_29 = 5029;
int lead_30 = 5030;
int lead_31 = 5031;
int lead_32 = 5032;
int lead_33 = 5033;
int lead_34 = 5034;
int lead_35 = 5035;
int lead_36 = 5036;
int lead_37 = 5037;
int lead_38 = 5038;
int lead_39 = 5039;
#define EXPR3(a, b, c) int w = a b c;
EXPR3(3, + 4, + 5)
