// RUN: %clang-refold-tester-verify-off structural_gap_straddle_dependency_pragma_commits_payload
// A payload straddling `#pragma GCC dependency` is committed across it.
//
// Clang consumes the directive: it emits nothing into the preprocessed stream,
// so alignment does not fix which side of it the payload `9` belongs on.
// `PragmaDependencyHandler` only looks the named file up and at most warns
// that it is newer than the current file.  With a quoted header name and
// nothing after it, the handler expands no macro, so the directive changes no
// state a payload could observe, and both sides are equivalent.
//
// This input used to pin the fail-closed default for an unclassified consumed
// pragma, as `structural_gap_straddle_unclassified_consumed_pragma_refuses`.
// The refusal was a classification gap, not a proof: both placements pass the
// closing check.  Verification is off so the planner alone must commit it.
int arr[] = { 
#pragma GCC dependency "gap_dependency.h"
9 };
