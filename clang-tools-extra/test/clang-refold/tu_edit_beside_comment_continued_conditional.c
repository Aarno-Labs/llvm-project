// RUN: %clang-refold-tester tu_edit_beside_comment_continued_conditional
//
// Regression: an edit in a translation unit whose `#if` is continued onto a
// second line by a block comment.  The producer's textual scan ended the
// directive at its first physical newline, which disagreed with the consumer's
// census of the same directive, so the group could not bind and every edit in
// the file refused.  The producer now records the extent Clang's lexer read.
int before = 1;
#if 1 /* this comment continues
         the directive */
int inside = 2;
#endif
int after = 3;
