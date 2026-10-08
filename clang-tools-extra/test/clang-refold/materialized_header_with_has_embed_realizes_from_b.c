// RUN: %clang-refold-tester-clang-flags-verify-off materialized_header_with_has_embed_realizes_from_b -- -I headers/materialized_header_has_embed
// `orig/a.h` must be inlined to realize its edit, and it evaluates
// `__has_embed("rmhe_data.txt")`, which finds the resource beside `orig/a.h`.
// Inlined source would search beside the output instead and select the other
// arm. Clang reports `__has_embed` through its own callback, not the
// `__has_include` one the conditional-arm flag is built from, so only the
// per-instance lookup fact sees it, and the header is realized from B.
#include "orig/a.h"
int main(void) { return rmhe_a + rmhe_v; }
