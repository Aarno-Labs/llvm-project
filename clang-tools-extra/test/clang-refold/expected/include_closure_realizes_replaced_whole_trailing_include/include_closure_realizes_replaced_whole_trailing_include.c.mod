// RUN: %clang-refold-tester include_closure_realizes_replaced_whole_trailing_include
// A TU/include closure that realizes exactly one hunk must replace it with the
// hunk's own B tokens.
//
// The edit rewrites the initializer so that the header's one token is not
// kept: the hunk replaces `1, 2, <1>` with `7, 1, 0`, and the closure that
// consumes the `#include` line is the only realizer that can own it.  Its B
// range used to come from the byte-diff cover projection, which pairs the
// repeated `1` differently and yields `1, 0`; the closure then refused the
// hunk it was built for, and the whole translation unit had no admissible
// refold.  The token edit script already names the B range, so the closure
// uses it.
int seven[] = { 7, 1, 0  };
int use(void) { return seven[0]; }
