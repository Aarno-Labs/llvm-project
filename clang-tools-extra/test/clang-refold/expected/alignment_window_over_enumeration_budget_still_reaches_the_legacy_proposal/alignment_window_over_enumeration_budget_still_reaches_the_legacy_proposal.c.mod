// RUN: %clang-refold-tester alignment_window_over_enumeration_budget_still_reaches_the_legacy_proposal
// RUN: FileCheck --input-file=%t/outputs/alignment_window_over_enumeration_budget_still_reaches_the_legacy_proposal.out %s
//
// An enumeration budget bounds the commit rules whose ground set it
// enumerates, not the window those rules sit in.  The legacy boundary
// proposal's ground set is only the core-optimal maps that carry the anchors
// its counterfactuals prove necessary, so a window whose complete enumeration
// exceeds the budget must still reach that rule.
//
// This is the input
// alignment_window_over_budget_still_reaches_the_legacy_proposal.c commits,
// with eight inserted wrappers instead of five.  Each wrapper repeats the
// `) ; }` tail that `first` already ends with, and the number of distinct
// optimal maps grows with them: 91, 140 and 204 for five, six and seven.  At
// eight, one forced-anchor sub-window alone exceeds the per-sub-window budget,
// so the observational-irrelevance and least-source-mutation rules, which
// quantify over every optimal map, cannot be decided.
//
// The legacy proposal is still a complete jointly core-optimal map, and its
// three counterfactuals prove three anchors necessary exactly as they do at
// five wrappers.  Conditioning the enumeration on those anchors splits the
// window at them, and exactly one optimal map carries all three.  The window
// commits that map's class: the last wrapper keeps the original closing brace
// line, which is the same choice the five-wrapper test pins.
//
// CHECK: exact map enumeration incomplete for A=[45,48) B=[65,244) through 72 anchor(s)
// CHECK: could not be enumerated within the proof budget; only the legacy boundary proposal, which enumerates the maps carrying its required anchors, remains decidable
// CHECK: 1 core-optimal map(s) carry the 3 required anchor(s)
// CHECK: committed one realized-source class: 1 of 1 required-anchor-carrying map(s) share it, 3 anchor(s) proved
#define NIL ((void*)0)

struct XjGlobals;

void push(struct XjGlobals *xjg, char c);
void pop(struct XjGlobals *xjg);
char top(struct XjGlobals *xjg);
char empty(struct XjGlobals *xjg);
int size(struct XjGlobals *xjg);

void *first(void)
{
  return NIL;
}
void *first_w(void *x, void *s) { return first_impl(x, s); }
void *first_v(void *x, void *s) { return first_impl(x, s); }
void *first_u(void *x, void *s) { return first_impl(x, s); }
void *first_t(void *x, void *s) { return first_impl(x, s); }
void *first_s(void *x, void *s) { return first_impl(x, s); }
void *first_r(void *x, void *s) { return first_impl(x, s); }
void *first_q(void *x, void *s) { return first_impl(x, s); }
void *first_p(void *x, void *s) { return first_impl(x, s);
}
static int second(int a, int b);

int tail_0 = 6000;
int tail_1 = 6001;
int tail_2 = 6002;
int tail_3 = 6003;
