// RUN: env CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET=200000 CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_COST_BUDGET=365 %clang-refold-tester alignment_window_retires_observational_irrelevance_when_decline_is_output_neutral
// RUN: FileCheck --input-file=%t/outputs/alignment_window_retires_observational_irrelevance_when_decline_is_output_neutral.out %s
//
// The observational-irrelevance rule commits a window exactly when every
// enumerated map realizes one concrete output, and its only prefix denial is a
// disagreement.  The window it would commit is therefore the window it must
// realize in full: kgabis/parson `tests.c` realized all 128 maps of one window
// over 19556 A tokens, about 495 s of a 527 s run.
//
// Declining the rule on the realization budget would bound that, but a decline
// can change the output, and the rule is not a cost policy.  It is retired only
// when its two verdicts are proved to emit one output.  Here the edit inside
// IS_DIGIT's argument admits 12 core-optimal maps in window 1, all realizing
// one output.  Committing emits that output.  Denying it leaves no rule: the
// least-source-mutation rule is over the same budget, and the legacy proposal
// is unreachable.  So the window would decline and keep its core-forced
// anchors, and no later window carries ambiguity.  One realization of the
// core-forced alignment matches the output the realized candidates share, so
// both verdicts emit the same source and the rule is retired after two
// candidates instead of twelve.  It is asked only once two realized maps agree,
// because a disagreement at the second map would deny the rule for nothing.
//
// The emitted source is the one the exhaustive enumeration commits to; the
// expected output below is byte-identical to a run at an unbounded budget.
// The budget is injected at 365, one realization's worth of this 365-token
// stream, because an input small enough to read cannot exceed the production
// bound.
//
// CHECK: window 1: core-forced alignment realized: accepted; its output matches the output every realized candidate shares
// CHECK: window 1 retires the observational-irrelevance rule after realizing 2 map(s): committing and declining emit one output, so realizing its 12 enumerated map(s) over 365 A token(s) (cost 4380, over the realization budget 365) cannot change it
// CHECK: alignment resolution probe finished: realized 3 candidate map(s) as complete refolds
extern const unsigned short int **ctype_table(void);

#define DIGIT_MASK 2u
#define CAST(type, value) ((type)(value))
#define IS_DIGIT(value) \
  ((*ctype_table())[(int) ((value))] & (unsigned short int) DIGIT_MASK)

int lead_0 = 5000;
int lead_1 = 5001;
int lead_2 = 5002;
int lead_3 = 5003;

struct holder {
  struct {
    char *s;
  } value;
};

int read_next(struct holder *m, char *s, unsigned s_index_xj) {
  return IS_DIGIT(CAST(unsigned char, *++s));
}

int keep_next(char *s) {
  return IS_DIGIT(CAST(unsigned char, *s));
}

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
