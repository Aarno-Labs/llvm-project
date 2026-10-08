// RUN: %clang-refold-tester-clang-flags-verify-off include_replay_rejects_copy_skipped_by_once_state -- -I headers/include_replay_copy_once_skip -I headers/include_replay_copy_once_skip/copy
// Inlining `orig/a.h` moves its skipped `#include "sub/leaf.h"` beside the
// output, where it reaches `copy/sub/leaf.h`, a guarded byte-identical copy.
// Entering such a copy reproduces a skip only if its guard is closed, but
// Clang skipped `leaf.h` here by its `#pragma once` with `RCOS_LEAF_H`
// undefined; the copy would be entered and emit `rcos_leaf` a second time.
// The copy does not stand for the producer's header, no macro test keeps the
// directive inert either, and `orig/a.h` is realized from B, which drops it.
#include "orig/sub/leaf.h"
#undef RCOS_LEAF_H
#include "orig/a.h"
int main(void) { return rcos_a + rcos_leaf; }
