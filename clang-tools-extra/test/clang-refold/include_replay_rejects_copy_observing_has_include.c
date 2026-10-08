// RUN: %clang-refold-tester-clang-flags-verify-off include_replay_rejects_copy_observing_has_include -- -I headers/include_replay_copy_has_include -I headers/include_replay_copy_has_include/copy
// As include_replay_accepts_guarded_byte_identical_copy, inlining `orig/a.h`
// moves `#include "sub/leaf.h"` out of `orig/`, and from the output it now
// reaches `copy/sub/leaf.h`, a guarded byte-identical copy. But `leaf.h`
// evaluates `__has_include("rchi_extra.h")`, which searches beside the file
// that contains it: false from `orig/sub/`, true from `copy/sub/`. Equal
// bytes are not equal tokens there, so the child's lookup-context obligation
// refuses the copy: it holds only for the producer's own file selected
// through the producer's lookup. The operand is rewritten to one that
// reaches `orig/sub/leaf.h` through `-I`, the search entry `orig/a.h` was
// found through and whose cursor `leaf.h` inherited.
#include "orig/a.h"
int main(void) { return rchi_a + rchi_leaf; }
