// RUN: %clang-refold-tester-with-lines-verify-off mixed_owner_include_closure_line_gap_before_preserved_pop_macro_resumes
//
// The closure consumes the `#line` and keeps the `#pragma pop_macro` in place,
// splitting the B payload around it.  The directive's filename operand
// evaluated to "pushed.c" where it was written, before the pop.  Kept with its
// own spelling after the replacement, it would follow the pop and evaluate to
// "gap c".  The replacement emitted ahead of the directive holds a directive,
// so its spelling is not kept, and a resume re-establishes the line state it
// computed.
#define FILE_NAME(x) #x
#pragma push_macro("FILE_NAME")
#undef FILE_NAME
#define FILE_NAME(x) "pushed.c"
int x =
1
#line 123 FILE_NAME(gap c)
+
#pragma pop_macro("FILE_NAME")
#include "two.inc"
;
int y = __LINE__;
const char *f = __FILE__;
