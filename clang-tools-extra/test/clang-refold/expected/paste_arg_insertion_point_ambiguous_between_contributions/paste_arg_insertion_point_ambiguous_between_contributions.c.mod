// RUN: %clang-refold-tester-verify-off paste_arg_insertion_point_ambiguous_between_contributions
// Regression: `foo_bar` -> `foo_x_bar` has two equally valid origins, because
// the `_` separating the two paste contributions repeats in the edited token.
// Reading the insertion at byte 4 attributes it to `b` (`M(foo, x_bar)`);
// reading it at byte 3 attributes it to `a` (`M(foo_x, bar)`).  Both re-expand
// to the edited token, so nothing in the token decides between them.
//
// The token-level differ maximized the common prefix and silently took the
// first, so the hunk now falls back to the expanded token instead of inventing
// one of the two origins.
//
// Two independent proofs cover this shape, and either alone makes this test
// pass: the witness-backed derivation reports an ambiguous origin and the
// caller fails closed rather than consulting the token-level differ, and the
// differ itself refuses to attribute an insertion whose position is not
// forced.  It is the first that carries this test in the current tree; the
// second is what covers shapes where no witness exists to prove ambiguity
// with, which no test in this suite currently reaches.
//
// Deliberately --verify-output=off.  At the harness default the closing check
// re-preprocesses the output, and *either* origin replays the edited stream --
// so a `fatal` variant passes whichever one the planner picks and cannot tell
// a proof from a rescue.
#define M(a,b) a ## _ ## b
int foo_x_bar = 0;
