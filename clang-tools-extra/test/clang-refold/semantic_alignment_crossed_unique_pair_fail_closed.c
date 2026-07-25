// RUN: %clang-refold-tester semantic_alignment_crossed_unique_pair_fail_closed
// A spelling that is individually unique on both sides is not necessarily a
// forced pair when an equally optimal crossed alignment exists.
#define ORIGINAL_EXPR a + b

int a = 1;
int b = 2;
int changed(void) { return ORIGINAL_EXPR; }
int separator = 909;
int untouched(void) { return ORIGINAL_EXPR; }
