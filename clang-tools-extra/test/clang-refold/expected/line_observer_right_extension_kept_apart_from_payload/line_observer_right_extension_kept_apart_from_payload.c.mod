// RUN: %clang-refold-tester-with-lines-verify-off line_observer_right_extension_kept_apart_from_payload
// Regression: B joins the `__LINE__` observer's line to the line before and
// the `SEMI` line after it, with no white space before the `;`.  The line
// observer layout realizes B's line up to the observer's `+ y`, then appends
// the following construct as source-spelled `SEMI` with B's empty gap, so
// the output read `501 + ySEMI`.  The appended source is now kept apart from
// the payload when the two would lex as one token.
#define LOC 500 "logical_right_macro_tu.c"
#define SEMI ;
enum { y = 2 };
#line LOC
int a = 1; int observed = 501 + y SEMI
