// RUN: %clang-refold-tester-verify-off sideband_pragma_equivalent_duplicates_in_certified_window_fold
// Two identical surviving pragmas certified to one window, with only one of
// them left in B, fold: both pairings print the same thing.
//
// Sideband directives are paired across the streams by the alignment-certified
// window they sit in rather than by their absolute normal-token gaps, because
// an edit beside a directive moves the gaps without moving the directive (see
// `surviving_pragma_adjacent_following_edit_preserves_directive`).  A window is
// coarser than a gap, so it can hold two directives that a gap would have told
// apart: here `- 22` is deleted, which un-anchors both tokens between the two
// `#pragma pack(1)` lines and leaves them at gaps 4 and 6 inside one window.
//
// B keeps one of the two, and nothing says which source directive it is.  The
// choice would be an order tie-break if the readings differed, but they do
// not: nothing but same-spelled lines separates the two in A, and B prints no
// normal token in the window.  Only A's deleted tokens lie between them, so
// either pairing prints one `#pragma pack(1)` between `1` and `+`.  This used
// to refuse the translation unit.
int a = 1
#pragma pack(1)
- 22
#pragma pack(1)
+ 3;
