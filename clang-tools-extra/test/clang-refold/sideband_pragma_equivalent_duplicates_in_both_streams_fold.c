// RUN: %clang-refold-tester-verify-off sideband_pragma_equivalent_duplicates_in_both_streams_fold
// Identical pragmas that share a certified window in both streams fold when
// only deleted tokens separate them.
//
// A has two `#pragma pack(1)` lines around `- 22`; B keeps both and drops the
// tokens.  Each stream then holds two same-key lines at different gaps of one
// window.  On each side nothing else prints between them, and the other
// stream prints no normal token in the window, so every pairing prints the
// same two lines between `1` and `+`.  This used to refuse the translation
// unit.
int b = 1
#pragma pack(1)
- 22
#pragma pack(1)
+ 3;
