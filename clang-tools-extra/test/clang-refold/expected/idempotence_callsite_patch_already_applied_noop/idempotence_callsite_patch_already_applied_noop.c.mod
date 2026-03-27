// RUN: %clang-refold-tester-with-lines idempotence_callsite_patch_already_applied_noop

// test87: Idempotence no-op after callsite patch already applied.
#define Q(T) int q_##T = 1;
Q(float)
Q(short)
int main(){ return q_float + q_short; }
