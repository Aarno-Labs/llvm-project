// RUN: %clang-refold-tester-verify-off structural_gap_straddle_skipped_unknown_directive_commits_payload
// A payload straddling an unrecognized directive in a skipped arm is committed
// across the whole gap.
//
// An unrecognized directive is an error outside a skipped group, so the only
// reachable form of this cell wraps it in `#if 0`.  A skipped group's
// directives are read only for their names, to track conditional nesting
// (C11 6.10.1p6), so the directive never executes.  The producer recorded its
// enclosing arm as not selected, which discharges it:
//
//   structure=OtherDirective ... crossable=true proof=SkippedArmDirective
//
// `#if 0` and `#endif` are crossed by the selected-arm rule, since the payload
// lands outside the group.
//
// This input used to be `structural_gap_straddle_unknown_directive_refuses`,
// which refused with `NoRuleForStructureKind`.  Verification is off so the
// planner alone must commit the payload.
int arr[] = { 
#if 0
#nonstandard directive
#endif
9 };
