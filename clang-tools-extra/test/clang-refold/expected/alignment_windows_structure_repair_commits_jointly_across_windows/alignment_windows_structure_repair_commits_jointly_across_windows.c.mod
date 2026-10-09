// RUN: env CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET=2000000 %clang-refold-tester alignment_windows_structure_repair_commits_jointly_across_windows
// RUN: FileCheck --input-file=%t/outputs/alignment_windows_structure_repair_commits_jointly_across_windows.out %s
//
// Regression: two certification windows whose core-forced maps both request
// terminal fallback are repaired together, because neither can be repaired
// alone.
//
// The budget splits this file into two certification windows.  Window 0
// deletes the `basis` declarations before the empty `PRIVATE` invocation, and
// its core-forced map widens the deletion through that invocation, which no
// owner covers.  Window 1 inserts `helper` after `finish`; its core-forced map
// replaces `; } char *` with the inserted function around the `PRIVATE` in
// front of `pathsearch`, which no owner covers either.  Each window's
// structure-respecting repair keeps one map, but a window's candidates are
// realized with every later window, and every window that declined, at its
// core-forced map, so each still carried the other's terminal request.
// Neither window committed, and the run ended in "no admissible refold".
// Window 0 now varies window 1's preferred sub-rectangle with its own, since
// its decline's terminal requests fall in both; the one joint candidate is
// accepted, and both windows commit.
//
// Shape of lemon.c in the tenjin corpus.
//
// CHECK: window 0: varying its preferred sub-rectangles jointly with later window(s) [1], whose core-forced anchors its decline's terminal requests also fall in: 1 candidate map(s)
// CHECK: window 0 committing joint structure-respecting repair with later window(s) [1]: declining requests terminal fallback
// CHECK: window 1 committed StructureRespectingTerminalRecovery jointly with window 0
// CHECK: window 1 needs no resolution of its own: an earlier window's structure-respecting repair committed it jointly
// CHECK: alignment resolution reproduced: production's concrete output equals witness 2's simulation
// CHECK-NOT: did not reproduce
#define PRIVATE
struct config { int x; };

PRIVATE struct config *newconfig(void) { return 0; }

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
int lead_40 = 5040;
int lead_41 = 5041;
int lead_42 = 5042;
int lead_43 = 5043;
int lead_44 = 5044;
int lead_45 = 5045;
int lead_46 = 5046;
int lead_47 = 5047;
int lead_48 = 5048;
int lead_49 = 5049;
int lead_50 = 5050;
int lead_51 = 5051;
int lead_52 = 5052;
int lead_53 = 5053;
int lead_54 = 5054;
int lead_55 = 5055;
int lead_56 = 5056;
int lead_57 = 5057;
int lead_58 = 5058;
int lead_59 = 5059;

static int finish(int *fp)
{
  fp[0] = 0;
  return fp[1];
}

/* Search for the name. */
        static int helper(const char *base, int c) {
    const char *result = base + c;
    if (!result) return -1;
    return (int)(result - base);
}

PRIVATE char *pathsearch(char *argv0)
{
  return argv0;
}
