// RUN: %clang-refold-tester-clang-flags include_replay_accepts_guarded_byte_identical_copy -- -I headers/include_replay_identical_copy -I headers/include_replay_identical_copy/copy
// Inlining `orig/a.h` to realize its edit moves `#include "leaf.h"` out of
// `orig/`. From the output's directory the operand now reaches
// `copy/leaf.h` through the search path, a different file holding exactly
// the bytes of `orig/leaf.h` under a whole-file include guard. Equal bytes
// lex to equal tokens, and the guard deduplicates whichever copy is entered,
// so the copy stands for the producer's header and the directive is kept
// rather than inlined. This is the relation tenjin's per-stage tree copies
// have to the tree the producer read.
#include "orig/a.h"
int main(void) { return rcopy_a + rcopy_leaf; }
