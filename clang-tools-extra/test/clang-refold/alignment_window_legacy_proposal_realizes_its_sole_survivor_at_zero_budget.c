// RUN: env CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_COST_BUDGET=0 %clang-refold-tester alignment_window_legacy_proposal_realizes_its_sole_survivor_at_zero_budget
// RUN: FileCheck --input-file=%t/outputs/alignment_window_legacy_proposal_realizes_its_sole_survivor_at_zero_budget.out %s
//
// The realization floor applies to the legacy boundary proposal's surviving
// set as well as to the enumeration.
//
// The input is the one
// alignment_window_over_budget_still_reaches_the_legacy_proposal.c commits: its
// least-source-mutation rule declines on the budget, its legacy boundary
// proposal survives with three required anchors and one surviving map, and the
// window commits that map's realization class.  Here the budget is injected at
// zero.  The 91-map enumeration is far above the floor, so the
// least-source-mutation rule still declines; the proposal's single surviving
// map is within it, so the legacy rule is still decided and the window commits
// the same class, and the output is the committing sibling's.
//
// A single survivor used to be declined at this budget, and the window kept
// only what core certification forced.  That was sound but gave up a decision
// that costs no more than realizing one more map, which is exactly what the
// floor exists to always pay for.
//
// CHECK: declines the least-source-mutation rule after realizing 2 map(s): realizing its 91 enumerated map(s) over 79 A token(s) costs 7189, over the containment realization budget (0)
// CHECK-NOT: keeps core-forced anchors
// CHECK: committing realized-source class: candidates=91 classMembers=[20] requiredAnchors=3
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
