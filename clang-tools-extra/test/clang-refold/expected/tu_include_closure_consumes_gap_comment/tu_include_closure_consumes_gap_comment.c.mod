// RUN: %clang-refold-tester tu_include_closure_consumes_gap_comment
// RUN: FileCheck --input-file=%t/outputs/tu_include_closure_consumes_gap_comment.out %s
//
// PINS DELIBERATE, DOCUMENTED BEHAVIOR -- not a defect.
//
// `RefoldExpansionFallbackPlanner.cpp` states the rule directly above the gap
// handler: "Complete comments and literal empty conditional-control islands are
// consumed with the replacement for the same reason as whitespace: they
// contribute no PP tokens and are physically inside the source interval whose
// A-side material was replaced by the B-side hunk."  The gap handler routes
// comments to the consuming branch on purpose, alongside four preserving
// branches for conditional-control tails, indexed trivia, zero-token pieces and
// source line directives.
//
// Erasing the `#include` is the same decision: an include closure exists to
// replace a contiguous run of top-level include directives plus the inert
// trivia between them, and here the header's only contribution is the token
// this edit deletes.
//
// This test exists so that decision is visible and cannot change silently.
//
// The edit spans the TU/include owner boundary, so the hunk is realized by
// `BuildTUIncludeClosureEditForUnresolvedHunk`, which replaces one contiguous
// source interval running from the TU part through the include part.  That
// interval contains the comment and the `#include` line sitting between them,
// and both are erased.  The refold *succeeds*: no terminal request, no warning,
// `terminalFallback=no`.
//
// Neither loss is visible to the closing verification, because neither a
// comment nor this include contributes anything to the preprocessed token
// stream, so `--verify-output` compares equal.  That is exactly why this is
// pinned by an assertion on the accept record rather than by output alone.
//
// The control for this test is an edit confined to the TU part: the same source
// with the same header keeps both the comment and the `#include`.  The loss is
// specific to a closure spanning the gap, not to the shape of the file.
//
// If the invariant "preserve every preprocessing directive outside an inlined
// `#include`" is meant to override this rule, that is a deliberate design
// change: this test's expected output would then keep both, under review.
//
// CHECK: TU include-closure accepted
// CHECK: refold summary: {{.*}} terminalFallback=no
int arr[] = { 3
};
