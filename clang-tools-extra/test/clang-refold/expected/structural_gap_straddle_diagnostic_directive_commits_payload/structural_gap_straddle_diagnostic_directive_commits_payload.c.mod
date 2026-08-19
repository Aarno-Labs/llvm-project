// RUN: %clang-refold-tester structural_gap_straddle_diagnostic_directive_commits_payload
// Regression: one B token replacing material on both sides of a `#warning` is
// committed after the directive instead of surrendering the translation unit.
//
// The payload's side is not fixed by alignment -- it replaces `1,` and `2`,
// which sit on opposite sides of the directive -- so the tiler must prove the
// two placements equivalent before it may choose one.
//
// A `#warning` emits a diagnostic, mutates no preprocessor state and produces
// no token, so there is nothing for either placement to observe.  That holds
// for every payload rather than for this one, which makes this the cheapest
// structure in the family to admit.  The gap-crossing proof reports
// `proof=StatelessDiagnosticDirective`, and the directive keeps its own line.
int arr[] = { 
#warning straddled
9 };
