// RUN: %clang-refold-tester macro_expansion_hunk_edge_widening_merges_hunk_sharing_expansion
// An edit that touches a macro argument and runs past the callsite must still
// refold, never drop the tokens past the cover and never fall back to raw B.
//
// `PAIR(a, b) * c` becomes `p + q / d`.  The token diff splits this into
// `a` -> `p` inside the expansion and `b * c` -> `q / d`, which begins on the
// expansion's last token and ends in plain translation-unit tokens: a straddle.
// Neither retraction applies, because the edge tokens differ on both sides, and
// widening the second hunk's left edge reaches the first hunk while still inside
// `PAIR`'s expansion.  Widening used to stop there, leaving no owner for the
// straddle.  The narrowing ladder then gave `PAIR` up, and the given-up-owner
// path emitted only `PAIR`'s expansion for the hunk: `int r = p + q * c;`, exit
// status 0 with verification off.  After that path learned to refuse, the
// refold failed closed instead.
//
// Widening now merges a neighbouring hunk that holds part of the same
// expansion, so the whole callsite plus the tokens past it form one hunk.  Tiling
// then realizes it as the args-only rewrite plus a TU edit, keeping `PAIR`.
#define PAIR(x, y) x + y

int probe(int a, int b, int c, int p, int q, int d) {
  int r = PAIR(p, q) / d;
  return r;
}
