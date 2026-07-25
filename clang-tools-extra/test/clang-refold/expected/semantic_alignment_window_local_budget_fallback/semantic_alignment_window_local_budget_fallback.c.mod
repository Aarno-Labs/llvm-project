// RUN: env CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET=55000 %clang-refold-tester semantic_alignment_window_local_budget_fallback
// RUN: FileCheck %s --check-prefix=WINDOW < %t/outputs/semantic_alignment_window_local_budget_fallback.out
// An over-budget repeated-token rectangle must lose only its local anchors;
// independently certified windows on both sides must retain their macro edits.
// WINDOW: seam index=0 A=40 B=40 proof=EveryOptimalPathCrossesState
// WINDOW: seam index=1 A=60 B=140 proof=EveryOptimalPathCrossesState
// WINDOW: window index=0 A=[0,40) B=[0,40) {{.*}}status=Certified
// WINDOW: window index=1 A=[40,60) B=[40,140) {{.*}}status=BudgetExceeded
// WINDOW: failed rectangle index=1 A=[40,60) B=[40,140) {{.*}}status=BudgetExceeded
// WINDOW: window index=2 A=[60,80) B=[140,160) {{.*}}status=Certified
// WINDOW: all windows certified=false
#define ID(x) x

int prefix_0 = ID(110);
int prefix_1 = ID(111);
int prefix_2 = ID(112);
int prefix_3 = ID(113);
int prefix_4 = ID(114);
int prefix_5 = ID(115);
int prefix_6 = ID(116);
int prefix_7 = ID(117);
int left_window_marker = 7001;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
int right_window_marker = 7002;
int suffix_0 = ID(220);
int suffix_1 = ID(221);
int suffix_2 = ID(222);
int suffix_3 = ID(223);
