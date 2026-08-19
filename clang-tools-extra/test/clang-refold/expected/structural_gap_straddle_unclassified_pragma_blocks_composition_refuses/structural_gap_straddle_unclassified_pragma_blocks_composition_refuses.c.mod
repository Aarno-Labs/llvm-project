// RUN: %clang-refold-tester structural_gap_straddle_unclassified_pragma_blocks_composition_refuses
// XFAIL: *
// Refusal shape: a gap whose structures are answered individually still
// refuses when one of them has no answer.
//
// This file was written to pin the *composition* of a gap holding two kinds,
// on the premise that a `#pragma` was simply "one directive of any other
// kind".  It is not: `region` is an unclassified spelling, which the taxonomy
// treats as changing arbitrary state, so this gap would refuse even if nothing
// else were in it -- exactly as
// `undetermined_payload_side_refuses_without_dropping_structure` does.
//
// Composition itself now works and is pinned positively by
// `structural_gap_straddle_composed_directive_kinds_commits_payload` and
// `structural_gap_straddle_error_in_skipped_arm_commits_payload`.  What this
// file pins is the other half: one unanswerable structure takes the whole gap
// down even when every other structure in it is proven crossable.  The trace
// shows both verdicts side by side,
//
//   structure=MacroDefine ... crossable=true  proof=MacroStateDirectiveUnobserved
//   structure=Pragma      ... crossable=false reason=PragmaNotConsumed
//
// and the refusal reaches the seam as
// `obligation=OwnerClosedCover reason=NoOwnerClosedCover stage=classify`.
//
// TRIAGE: incompleteness, deferred to the pragma-classification package.  This
// cell folds as soon as `region` is classified from Clang's own pragma
// handlers, and it should then stop being expected to fail.
int arr[] = { 
#define GAP_A 1
#pragma region
9 };
