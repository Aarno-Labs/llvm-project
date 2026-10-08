// RUN: %clang-refold-tester-verify-off structural_gap_straddle_skipped_define_commits_payload
// RUN: FileCheck --input-file=%t/outputs/structural_gap_straddle_skipped_define_commits_payload.out %s
// A payload straddling a `#define` in a skipped arm is committed across the
// whole gap, even when the payload spells the name it would define.
//
// A skipped group's directives are read only for their names, to track
// conditional nesting (C11 6.10.1p6), so `#  define h 3` never executes.  It
// binds nothing, and the producer has no record of it.  That missing record
// used to leave the gap's macro-state binding set incomplete, which rejected
// the directive with `NoProducerRecord` and surrendered the translation unit.
// The producer recorded the innermost arm holding it -- `# if 1`, itself
// inside `#if 0` and never evaluated -- as not entered, which discharges it:
//
// CHECK: structure=MacroDefine source=[{{[0-9]+}},{{[0-9]+}}) crossable=true proof=SkippedArmDirective
//
// The conditional controls are crossed by the selected-arm rule, since the
// payload lands outside the group, where `h` stays an identifier.
// Verification is off so the planner alone must commit the payload.
//
// Regression for file's softmagic.c, where a function inserted between a
// function body and `#if !defined(HAVE_STRNDUP)`, whose skipped arm holds
// `#  define strndup aix_strndup`, ended in the whole-file carrier.
int h = 4;
int arr[] = { 1,
#if 0
# if 1
#  define h 3
# endif
#endif
2 };
