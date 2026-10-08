// RUN: %clang-refold-tester-clang-flags-verify-off include_replay_rejects_cursor_change_under_has_include_next -- -I headers/include_replay_has_include_next_cursor/d0 -I headers/include_replay_has_include_next_cursor/sub
// `sub/p.h` is reached relative to this file, so the `h.h` it includes beside
// itself inherits no search-chain cursor, and `__has_include_next` in `h.h`
// searches the whole chain and finds `d0/rhnc_x.h`. Inlining `p.h` moves
// `#include "h.h"` into the output, which reaches the very same `sub/h.h`
// through `-I .../sub`, the second search entry; from there the search
// resumes after `sub/` and finds nothing. The physical file is the producer's,
// but its lookup is not, so the child is realized from B rather than kept.
#include "headers/include_replay_has_include_next_cursor/sub/p.h"
int main(void) { return rhnc_a + rhnc_h; }
