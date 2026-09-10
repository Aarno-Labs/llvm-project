// RUN: %clang-refold-tester alignment_window_over_budget_still_reaches_the_legacy_proposal
// RUN: FileCheck --input-file=%t/outputs/alignment_window_over_budget_still_reaches_the_legacy_proposal.out %s
//
// A realization budget bounds one commit rule, not the window that rule sits
// in.  Declining the rule whose set is the whole enumeration must still leave
// the window to the rules whose set is small.
//
// The inserted wrappers repeat the `) ; }` tail that `first` already ends with,
// so core certification cannot force the window's anchors and the all-optimal
// enumeration returns 91 complete maps.  Each realizes a different source edit,
// so the observational-irrelevance rule dies at the second distinct concrete
// output, and the least-source-mutation rule -- which admits no denial from a
// prefix and would cost one whole-translation-unit realization per enumerated
// map -- declines on its realization budget after two.
//
// Unlike alignment_window_declines_when_containment_exceeds_realization_budget.c,
// the legacy boundary proposal here *is* a complete jointly core-optimal map.
// Its set is the proposal, its leave-one-out counterfactuals, and the maps that
// carry the anchors those prove necessary -- three anchors and one surviving
// map -- so it is decidable for six realizations rather than 91, and the window
// commits the same class either way.
//
// Distilled from the piotrl/c_markdown `stack.c` refold, where 193 enumerated
// maps were realized so that two rules that could not fire could be asked, and
// the rule that committed read one of them.
//
// CHECK: declines the least-source-mutation rule after realizing 2 map(s): its 91 enumerated map(s)
// CHECK: candidate census: enumerated=91 realized=2
// CHECK: committing realized-source class: candidates=91 classMembers=[20]
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
void *first_s(void *x, void *s) { return first_impl(x, s);
}
static int second(int a, int b);

int tail_0 = 6000;
int tail_1 = 6001;
int tail_2 = 6002;
int tail_3 = 6003;
