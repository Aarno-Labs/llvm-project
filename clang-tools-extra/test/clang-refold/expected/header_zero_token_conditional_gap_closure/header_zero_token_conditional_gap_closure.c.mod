// RUN: %clang-refold-tester header_zero_token_conditional_gap_closure
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int before = 10,
#if 0
int disabled = SHOULD_NOT_APPEAR;
#endif
 after = 20;
