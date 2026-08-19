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
// This is the positive half of the composition contract.  The negative half is
// `structural_gap_straddle_unknown_directive_refuses`, where two crossable
// conditional controls surround one unrecognized directive and the whole gap
// must still refuse; if that test ever passes, the tiler is admitting a gap it
// has not decomposed.
int arr[] = { 1,
#define COMPOSED_GAP_A 1
#warning straddled
2 };
