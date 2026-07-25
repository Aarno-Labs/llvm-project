// RUN: %clang-refold-tester semantic_alignment_repeated_pair_partial_edit
// Repeated equal tokens from one actual must not make a partial expansion edit
// look like a sound argument rewrite of either identical macro occurrence.
#define PAIR(x) x, x

int values[] = { PAIR(1), 909, PAIR(1) };
