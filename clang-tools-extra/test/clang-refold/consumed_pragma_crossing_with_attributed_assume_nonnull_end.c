// RUN: %clang-refold-tester-verify-off consumed_pragma_crossing_with_attributed_assume_nonnull_end
// A consumed-pragma crossing folds when the translation unit also prints
// `#pragma clang assume_nonnull end`.
//
// Crossing a preserved pragma requires that the preprocessor emitted nothing
// for it.  The producer records that as the absence of an image in A, and the
// absence counts only when every printed pragma was bound to its item
// (`pragma_images_complete`).  Clang used to hand
// `PPCallbacks::PragmaAssumeNonNullEnd` an invalid `SourceLocation`, so the
// printed `end` below could not be bound, and the whole translation unit fell
// back to the fail-closed answer for the unrelated `region` crossing:
//
//   structure=Pragma ... crossable=false reason=PragmaNotConsumed
//
// The callback now receives the directive's own location, as `begin` always
// did, so every emission is accounted for and the `region` crossing is proven
// exactly as in `consumed_pragma_straddle_commits_payload_and_preserves_directive`.
// This input used to be `pragma_image_accounting_incomplete_refuses_crossing`.
// Verification is off so the planner alone must commit the payload.
int arr[] = { 1,
#pragma region
2 };
#pragma clang assume_nonnull begin
int guarded;
#pragma clang assume_nonnull end
