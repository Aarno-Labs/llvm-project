// RUN: %clang-refold-tester-clang-flags include_replay_rejects_copy_with_rewritten_nested_header -- -I headers/include_replay_copy_closure -I headers/include_replay_copy_closure/copy
// As include_replay_accepts_guarded_byte_identical_copy, `#include "leaf.h"`
// leaves `orig/` and now reaches `copy/leaf.h`, byte-identical to
// `orig/leaf.h`. But the copy's own `#include "deep.h"` resolves beside it to
// `copy/deep.h`, whose bytes differ from the `orig/deep.h` the producer
// read. A copy stands for the producer's header only when every include it
// executes does the same, so `leaf.h` is inlined, and so is `deep.h`.
#ifndef RCLOS_LEAF_H
#define RCLOS_LEAF_H
#ifndef RCLOS_DEEP_H
#define RCLOS_DEEP_H
int rclos_deep = 4;
#endif
int rclos_leaf = 2;
#endif
int rclos_a = 3;
int main(void) { return rclos_a + rclos_leaf + rclos_deep; }
