// RUN: %clang-refold-tester-clang-flags include_next_comment_continued_directive_relocated -- -I headers/include_next_comment_continued/a -I headers/include_next_comment_continued/b
//
// Regression: an `#include_next` continued onto a second physical line by a
// block comment is rewritten to an ordinary `#include` when its header is
// materialized, rather than being expanded.  A physical-line splice scan ends
// the directive inside the comment and cannot find the operand; the producer
// records the directive's extent, keyword and operand as Clang lexed them.
int shim_value_cc = 41;
#include /* resume the search after
                 this directory */ <leaf.h>
int total_cc = shim_value_cc + leaf_value_cc;
