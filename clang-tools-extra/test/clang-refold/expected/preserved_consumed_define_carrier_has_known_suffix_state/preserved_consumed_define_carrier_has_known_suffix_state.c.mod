// RUN: env CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET=2000000 %clang-refold-tester preserved_consumed_define_carrier_has_known_suffix_state
// RUN: FileCheck --input-file=%t/outputs/preserved_consumed_define_carrier_has_known_suffix_state.out %s
//
// Regression: a TU edit that consumes a `#define` and re-emits it for a
// surviving callsite has a known suffix state, so its proof no longer vetoes
// an unrelated window's alignment repair.
//
// The budget splits this file into two certification windows, each holding
// deleted declarations whose leading tokens repeat after them.  In window 0
// the core-forced map widens the deletion through the empty `PRIVATE`
// invocation, which no owner covers, so declining requests terminal fallback
// and the structure-respecting repair must commit.  In window 1 the
// core-forced map consumes `#define ISOPT`, which the liveness repair
// re-emits for the surviving `ISOPT` callsite.
//
// Each window's candidates are realized with the other at its core-forced
// map.  The re-emitting edit used to carry no suffix-stability witness, so its
// suffix state was unknown and window 0's only preferred map was refused as
// proof-incomplete; window 1's in turn inherited window 0's terminal hunk.
// Neither could commit and the run ended in "no admissible refold".  The
// carrier now carries the witness of the transition it re-emits, window 0
// commits, and window 1's core-forced realization is admissible as it stands.
//
// CHECK: suffix_state=owner_realization:evidence=TUSpecializedRealization:state_witnesses=1:state={{[^:]*}}:StateRepair/MacroState
// CHECK: window 0 committing structure-respecting repair: declining requests terminal fallback
// CHECK: window 1 has no structure-respecting repair: its core-forced alignment accepted, so declining is admissible
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
                     
                          
static
#define ISOPT(X) ((X)[0] == '-')
 void report(void) { g_argv[0] = ISOPT(g_argv[0]) ? 0 : g_argv[0]; }
