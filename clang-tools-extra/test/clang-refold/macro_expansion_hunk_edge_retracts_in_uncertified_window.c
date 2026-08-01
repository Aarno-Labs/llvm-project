// RUN: env CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET=200000 %clang-refold-tester macro_expansion_hunk_edge_retracts_in_uncertified_window
// A hunk edge that falls strictly inside a macro expansion must retract to a
// whole-expansion boundary before the edit is planned.
//
// The certification partition here holds several separately certified windows,
// so the all-optimal semantic oracle is unavailable (it is retained only for
// the single complete-stream window).  Core certification forces the anchors
// it can prove, but the `) ; }` tail that B repeats in the inserted wrapper is
// not core-forced, so the raw optimal map may align A's `first` tail against
// the wrapper's tail.  That leaves the hunk starting inside `NIL`'s expansion,
// whose owner cannot realize a partial expansion; the surviving `((void*)0)`
// suffix bytes were emitted without their macro owner as `return );`.
//
// Retraction walks both edges outward across identical tokens until no edge
// splits an expansion, so the edit lands on a proven owner boundary and `NIL`
// is preserved.  Regression for the dbcc `mpc.c` refold defect.
#define NIL ((void*)0)

int lead_0 = 5000;
int lead_1 = 5001;
int lead_2 = 5002;
int lead_3 = 5003;

void *first(void)
{
  return NIL;
}
static int second(int a, int b);

int tail_0 = 6000;
int tail_1 = 6001;
int tail_2 = 6002;
int tail_3 = 6003;
int tail_4 = 6004;
int tail_5 = 6005;
int tail_6 = 6006;
int tail_7 = 6007;
int tail_8 = 6008;
int tail_9 = 6009;
int tail_10 = 6010;
int tail_11 = 6011;
int tail_12 = 6012;
int tail_13 = 6013;
int tail_14 = 6014;
int tail_15 = 6015;
int tail_16 = 6016;
int tail_17 = 6017;
int tail_18 = 6018;
int tail_19 = 6019;
int tail_20 = 6020;
int tail_21 = 6021;
int tail_22 = 6022;
int tail_23 = 6023;
int tail_24 = 6024;
int tail_25 = 6025;
int tail_26 = 6026;
int tail_27 = 6027;
int tail_28 = 6028;
int tail_29 = 6029;
int tail_30 = 6030;
int tail_31 = 6031;
int tail_32 = 6032;
int tail_33 = 6033;
int tail_34 = 6034;
int tail_35 = 6035;
int tail_36 = 6036;
int tail_37 = 6037;
int tail_38 = 6038;
int tail_39 = 6039;
int tail_40 = 6040;
int tail_41 = 6041;
int tail_42 = 6042;
int tail_43 = 6043;
