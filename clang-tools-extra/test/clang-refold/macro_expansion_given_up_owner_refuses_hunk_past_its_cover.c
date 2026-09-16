// RUN: %clang-refold-tester-expect-refold-fail-verify-off macro_expansion_given_up_owner_refuses_hunk_past_its_cover
// Soundness regression: emitting a given-up macro invocation's expansion
// realizes only that invocation's cover, so it must not stand in for a hunk
// that runs past the cover.
//
// `PAIR(a, b) * c` becomes `p + q / d`.  The edit splits into `a` -> `p` inside
// the expansion and `b * c` -> `q / d`, which begins on the expansion's last
// token and ends in plain translation-unit tokens.  No owner covers the second
// hunk, and no edge repair applies: its edge tokens differ on both sides, and
// the first hunk stops its left edge from widening.  The narrowing ladder names
// `PAIR`, which owns part of the failing range, and retries with it given up.
// The retry then emitted `PAIR`'s expansion for the second hunk and took the
// hunk as realized, although that expansion produces only `p + q`: the output
// was `int r = p + q * c;`, with exit status 0.
//
// Every other driver passes `--verify-output=fatal`, whose closing check does
// reject that output, so only the verify-off driver shows the planner itself
// accepting it.  This asserts that the refold exits non-zero.
//
// TRIAGE: incompleteness once it refuses.  One replacement consuming the whole
// callsite would realize the edit, but the edge repair's widening stops at the
// neighbouring hunk rather than merging the two.
#define PAIR(x, y) x + y

int probe(int a, int b, int c, int p, int q, int d) {
  int r = PAIR(a, b) * c;
  return r;
}
