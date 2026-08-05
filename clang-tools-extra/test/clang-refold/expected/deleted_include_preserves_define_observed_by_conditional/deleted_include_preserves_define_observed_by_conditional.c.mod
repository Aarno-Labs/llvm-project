// RUN: %clang-refold-tester deleted_include_preserves_define_observed_by_conditional
// The definition this include contributes is never expanded: its only observer
// is a conditional.  Liveness driven by surviving *callsites* alone would not
// see it, and dropping the include would flip the selected arm.
#define STATE_FEATURE 1

#ifdef STATE_FEATURE
int state_cond_on = 1;
#else
int state_cond_off = 2;
#endif
