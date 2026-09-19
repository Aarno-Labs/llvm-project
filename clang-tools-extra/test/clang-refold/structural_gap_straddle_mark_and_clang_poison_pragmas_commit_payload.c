// RUN: %clang-refold-tester-verify-off structural_gap_straddle_mark_and_clang_poison_pragmas_commit_payload
// A payload straddling two consumed pragmas is committed across both.
//
// `#pragma mark` is read raw to the end of the line and handed to a callback,
// so it changes nothing.  `#pragma clang poison` is the handler Clang also
// registers as `GCC poison`, and it is observed only by a payload naming a
// poisoned identifier; `9` names none.  Neither prints an image, so alignment
// does not fix the payload's side, and each must be proven crossable on its
// own.  Verification is off so the planner alone must commit the payload.
int arr[] = { 1,
#pragma mark gap section
#pragma clang poison gap_poisoned
2 };
