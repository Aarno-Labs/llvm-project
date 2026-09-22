// RUN: %clang-refold-tester-with-lines-verify-off mixed_owner_include_closure_line_gap_after_redefining_include_resumes
//
// The closure consumes both includes and the `#line` between them.  The first
// header redefines FILE_NAME, so the directive's filename operand evaluated to
// "hdr.c" where it was written.  Kept with its own spelling after the
// replacement, it would see the TU's definition instead, because the header
// is no longer included.  The consumed source ahead of the directive holds a
// directive, so its spelling is not kept, and a resume re-establishes the
// line state it computed.
#define FILE_NAME(x) #x
int x =
#undef FILE_NAME
5
#line 124 "hdr.c"
;
int y = __LINE__;
const char *f = __FILE__;
