// RUN: env CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_BUDGET=0 %clang-refold-tester alignment_window_legacy_proposal_declines_on_realization_budget
// RUN: FileCheck --input-file=%t/outputs/alignment_window_legacy_proposal_declines_on_realization_budget.out %s
//
// The realization budget is a completeness policy, not a proof, and this pins
// what exhausting it is allowed to cost.
//
// The input is the one
// alignment_window_over_budget_still_reaches_the_legacy_proposal.c commits: its
// least-source-mutation rule declines on the budget, its legacy boundary
// proposal survives with three required anchors and one surviving map, and the
// window commits that map's realization class.  Here the budget is injected at
// zero, so the surviving set exceeds it too and the legacy rule declines as
// well.  No rule is left, and the window keeps exactly what core certification
// forced.
//
// Two things must hold and both are asserted below.  The window's decline is
// reported against the surviving set, not against the enumeration -- the two
// rules are bounded separately and reach the budget for separate reasons.  And
// the refold still succeeds: the emitted source is the core-forced answer,
// which is checked here the way every other refold is, by diffing the whole
// output and re-preprocessing it against the edited stream.  Compare that
// output with the committing one to see what the budget bought and what it
// cost: the resolution is different, the soundness is not.
//
// Raising the budget can therefore only resolve more windows and lower it only
// fewer.  Neither direction can change a committed answer, which is why the
// value is a tuning decision rather than a theorem.
//
// CHECK: declines the least-source-mutation rule after realizing 2 map(s): its 91 enumerated map(s) exceed the containment realization budget (0)
// CHECK: keeps core-forced anchors: 1 map(s) surviving the 3 required anchor(s) exceed the realization budget (0)
#define NIL ((void*)0)

void push(char c);
void pop(void);
char top(void);
char empty(void);
int size(void);

void *first(void)
{
  return NIL;
}
static int second(int a, int b);

int tail_0 = 6000;
int tail_1 = 6001;
int tail_2 = 6002;
int tail_3 = 6003;
