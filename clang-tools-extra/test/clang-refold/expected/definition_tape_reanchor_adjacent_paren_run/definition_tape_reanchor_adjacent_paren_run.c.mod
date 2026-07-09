// RUN: %clang-refold-tester definition_tape_reanchor_adjacent_paren_run
#define M(x) ((x) >= 0)
int f(int y) { if (M((y).field)) return 1; return 0; }
