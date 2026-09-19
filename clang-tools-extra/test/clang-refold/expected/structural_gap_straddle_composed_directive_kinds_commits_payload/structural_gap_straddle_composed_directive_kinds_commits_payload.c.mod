// RUN: %clang-refold-tester structural_gap_straddle_composed_directive_kinds_commits_payload
// Regression: a gap holding two directives of *different* kinds is crossed by
// composing the two per-structure answers, not by a rule that recognizes the
// pair.
//
// A gap holding two macro directives already folded, because one macro-state
// verdict covers both.  This gap holds a `#define` and a `#warning`, which are
// answered by different theorems -- `MacroStateDirectiveUnobserved` and
// `StatelessDiagnosticDirective` -- and the crossing is admitted exactly
// because each of them is.
//
// This is the positive half of the composition contract.  The negative half,
// one uncrossable structure refusing the whole gap, was pinned by an
// unrecognized directive in a skipped arm until that became crossable
// (`structural_gap_straddle_skipped_unknown_directive_commits_payload`).
int arr[] = { 
#define COMPOSED_GAP_A 1
#warning straddled
9 };
