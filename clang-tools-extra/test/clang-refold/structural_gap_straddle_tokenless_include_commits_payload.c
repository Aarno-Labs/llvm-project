// RUN: %clang-refold-tester structural_gap_straddle_tokenless_include_commits_payload
// Regression: one B token replacing material on both sides of an `#include`
// that contributes no token is committed after the directive.
//
// The same include already folded when its header contributed even one token,
// because the include's own B envelope then anchors the payload's side.  With a
// token-free header there is no envelope and the include is just another
// zero-image structure in the gap, so the placement needs its own proof.
//
// What discharges it is the producer's record of the include instance rather
// than anything about the payload: the instance contributes no A token, and no
// macro directive, pragma, line control, conditional group or nested include is
// attributed to it, so entering it changed nothing either placement could
// observe.  The gap-crossing proof reports `proof=StatelessIncludeInstance`.
// An include that carries any of those keeps failing closed.
//
// The header is shared with no other test so its emptiness stays part of this
// regression.
int arr[] = { 1,
#include "structural_gap_tokenless.h"
2 };
