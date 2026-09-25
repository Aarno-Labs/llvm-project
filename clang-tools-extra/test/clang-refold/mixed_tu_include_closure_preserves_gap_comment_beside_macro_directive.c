// RUN: %clang-refold-tester mixed_tu_include_closure_preserves_gap_comment_beside_macro_directive
// A mixed TU/include closure keeps the comments of a gap whose macro definition
// it preserves in place.
//
// The replacement spans the comment and the `#define` between `1,` and the
// included `2`.  The definition is kept at its original position, and the
// comment -- which carries no token -- is re-emitted with it rather than
// deleted with the replaced envelope.  Regression for tenjin's
// localize_mutable_globals edits after a header, where a documented `#define`
// block between the `#include` and the edited definition disappeared.
int arr[] = { 1,
/* The value after the included one. */
#define GAP_VALUE 99
#include "two.inc"
};
