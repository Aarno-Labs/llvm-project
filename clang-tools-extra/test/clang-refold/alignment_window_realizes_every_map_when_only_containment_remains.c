// RUN: env CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET=200000 %clang-refold-tester alignment_window_realizes_every_map_when_only_containment_remains
// RUN: FileCheck --input-file=%t/outputs/alignment_window_realizes_every_map_when_only_containment_remains.out %s
//
// A certification window stops enumerating as soon as no commit rule can still
// fire, and only a denial that carries from the realized prefix to the
// completed rule may end one.  The least-source-mutation rule has no such
// denial.  Its least element must be no more destructive than *every* accepted
// map, so a prefix holding no such map proves only that none *already
// realized* is one; writing L(S) for the least set over S, a prefix P of the
// ground set G satisfies L(G) INTERSECT P is a subset of L(P), and a map still
// unrealized may be the least element the completed rule commits on.
//
// The inserted wrappers repeat the `) ; }` tail that `first` already ends
// with, so core certification cannot force the window's anchors and the
// all-optimal enumeration returns 14 complete maps.  Each realizes a different
// source edit, so the observational-irrelevance rule dies at the second
// distinct concrete output, and the legacy boundary proposal -- decided from
// the lexemes, the gap provenance, and the core alignment alone -- is
// unreachable here.  Containment is the one rule left, and its 14 candidates
// are inside the realization budget.
//
// This asserts that all 14 are realized and that the completed rule, not a
// prefix standing in for it, is what declines the window.  The two maps whose
// incomparability once ended the enumeration decide nothing on their own.
//
// CHECK: candidate census: enumerated=14 accepted=14
// CHECK: has no unique least source-mutation class: accepted=14
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
