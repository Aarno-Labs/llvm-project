// RUN: %clang-refold-tester-with-lines ambiguity_two_macros_same_expansion_edit_inside_run

// test88: Two macros expand to same tokens; edit inside that token run.
#define A88(x) int same88 = x;
#define B88(x) int same88 = x;
int same88_new = 1;
int main(){ return same88_new; }
