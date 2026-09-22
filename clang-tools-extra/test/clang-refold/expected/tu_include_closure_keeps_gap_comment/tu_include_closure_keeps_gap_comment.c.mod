// RUN: %clang-refold-tester tu_include_closure_keeps_gap_comment
// RUN: FileCheck --input-file=%t/outputs/tu_include_closure_keeps_gap_comment.out %s
//
// The edit spans the TU/include owner boundary, so the hunk is realized by
// `BuildTUIncludeClosureEditForUnresolvedHunk`, which replaces one contiguous
// source interval running from the TU part through the include part.  The
// comment lies in the gap between those parts.
//
// The gap is ordinary trivia, and a comment contributes no PP token, so the
// closure may consume the gap's bytes -- but a comment is source text, and the
// refold must not lose it.  The closure re-emits it after the replacement.
// Before this was fixed the comment was erased with the gap, and nothing saw
// it: the closing verification compares preprocessed tokens only.
//
// The `#include` is still erased.  The header's only contribution is the token
// this edit deletes, and an include closure exists to replace a contiguous run
// of top-level include directives.
//
// CHECK: TU include-closure accepted
// CHECK: refold summary [production attempt 0]: {{.*}} terminalFallback=no
int arr[] = { 3
// GAP_COMMENT_KEPT_BY_THE_CLOSURE
};
