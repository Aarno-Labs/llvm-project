// RUN: %clang-refold-tester semantic_alignment_identical_macro_bodies_partial_edit
// Identical replacement lists from different macro identities must not let an
// edit in the left expansion migrate to the right invocation.
#define LEFT_DUP(x) ((x) + (x))
#define RIGHT_DUP(x) ((x) + (x))

int left_value = ((3) + (4));
int separator = 909;
int right_value = RIGHT_DUP(3);
