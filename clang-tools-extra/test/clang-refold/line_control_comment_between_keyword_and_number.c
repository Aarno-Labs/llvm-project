// RUN: %clang-refold-tester-with-lines line_control_comment_between_keyword_and_number
// `#line/*c*/42` is legal C: the directive is recognized after comments are
// replaced with whitespace, so it sets the logical line exactly as `#line 42`
// does.  This pins that the line-control model agrees with the preprocessor on
// that spelling.
//
// B inserts a line ahead of the `__LINE__` observer, so the refold cannot
// simply preserve the source: it must emit a `#line` resume computed from the
// active source-authored line-control state.  Getting that resume right
// requires having recognized the comment-bearing directive -- a scanner that
// rejects the spelling would compute the observer's logical line from the file
// start instead of from line 42 and emit the wrong resume, which the closing
// verification would then reject.
//
// The recognizer used for this computation replaces comments before matching,
// so the spelling is handled.  Two other line-control scanners do read raw
// source and would miss it, but both are conservative there: the
// entry-wrapper minimizer keeps its synthetic `#line`, and
// `LineDirectiveWouldBeNoOp` returns "unknown", which only costs a redundant
// directive.  This test pins the semantic result, not those two.
#define WRAP(x) ((x) + 1)
#line/*c*/42
int a = WRAP(7);
int b = __LINE__;
