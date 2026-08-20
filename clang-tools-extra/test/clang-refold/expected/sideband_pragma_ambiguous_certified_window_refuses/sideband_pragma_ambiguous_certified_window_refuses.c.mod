// RUN: %clang-refold-tester sideband_pragma_ambiguous_certified_window_refuses
// XFAIL: *
// Refusal shape: two identical surviving pragmas certified to one window, and
// only one of them left in B.
//
// Sideband directives are paired across the streams by the alignment-certified
// window they sit in rather than by their absolute normal-token gaps, because
// an edit beside a directive moves the gaps without moving the directive (see
// `surviving_pragma_adjacent_following_edit_preserves_directive`).  A window is
// coarser than a gap, so it can hold two directives that a gap would have told
// apart: here `- 22` is deleted, which un-anchors both tokens between the two
// `#pragma pack(1)` lines and leaves them at gaps 4 and 6 inside one window.
//
// B keeps one of the two.  Nothing says which source directive it is.  Both
// readings replay B exactly and each deletes a different line of the source, so
// choosing between them would be a tie-break on key order -- the deterministic
// ordering that is explicitly not proof.  The pairing refuses the window
// instead:
//
//   refusing sideband pairing: certified window 4 holds two directives
//   spelled '#pragma pack(1)' at normal-token gaps 4 and 6
//
// TRIAGE: correct, and it must never pass while the input stays this shape.
// It is the fail-closed half of the certified-window pairing: the coordinate is
// deliberately weaker than an exact gap, and this is the ambiguity that buys.
//
// The guard is coarser than the ambiguity strictly requires -- it also refuses
// a shared window whose directives are all matched on both sides, where the
// order-preserving pairing is forced and no edit is emitted for either.  That
// costs nothing today: the exact-gap projection this replaced refused every
// such input as well, so no fold is withdrawn.  Sharpening it is a matching
// question, not a pairing one.
//
// The expected refold records one of the two admissible readings, deleting the
// second directive.  Deleting the first is equally consistent with B, which is
// the whole point.
int a = 1
#pragma pack(1)
+ 3;
