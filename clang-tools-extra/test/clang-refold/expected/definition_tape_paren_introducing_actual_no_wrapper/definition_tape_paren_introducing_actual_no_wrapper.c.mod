// RUN: %clang-refold-tester definition_tape_paren_introducing_actual_no_wrapper
// Companion to definition_tape_reanchor_adjacent_paren_run: the same macro and
// paren-introducing actual edit ((y) -> ((y).field)), but WITHOUT the enclosing
// if(...) that creates the ambiguous run of identical '(' tokens. The rewritten
// actual must still refold structure-preserving through the definition-tape
// replay rather than materializing the expansion.
#define M(x) ((x) >= 0)
int b = M((y).field);
