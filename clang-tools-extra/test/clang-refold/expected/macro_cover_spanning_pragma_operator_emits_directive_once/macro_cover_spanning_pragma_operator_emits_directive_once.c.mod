// RUN: %clang-refold-tester macro_cover_spanning_pragma_operator_emits_directive_once
// A whole cover whose own expansion straddles a `_Pragma` operator materializes
// the directive, because the cover is what produced it.
//
// The companion case, `macro_owned_gap_edit_before_surviving_pragma_emits_directive_once`,
// ends where the directive begins and must not carry it.  The two differ by
// where the directive's replay bytes sit relative to the cover's tokens, not by
// how the directive is spelled, and the rule is stated that way: a token range
// realizes the bytes its tokens occupy, so trivia past the last token is not
// its to emit while bytes between two of its tokens are.
//
// Here `SPAN` expands to `1,` then the operator's `#pragma pack(1)` line then
// `2`, so the directive's replay bytes lie between two cover tokens.  Replacing
// the invocation removes the only thing that executed the operator, and the
// realized text reproduces it: one directive in, one directive out.  A rule
// that refused any cover overlapping sideband bytes -- rather than one that
// distinguishes the trailing edge -- would refuse this fold instead.
//
// This case folded before the trailing-edge fix as well; it is pinned so the
// fix's boundary is a regression, not a comment.
#define SPAN 1, _Pragma("pack(1)") 2
int arr[] = { 5,
#pragma pack(1)
              2 };
