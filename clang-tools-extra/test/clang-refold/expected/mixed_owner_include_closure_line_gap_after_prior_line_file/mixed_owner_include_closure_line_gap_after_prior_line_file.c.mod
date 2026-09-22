// RUN: %clang-refold-tester-with-lines-verify-off mixed_owner_include_closure_line_gap_after_prior_line_file
//
// The closure consumes a `#line` that names no file, so it keeps "prior.c"
// from the untouched directive ahead of the closure.  The consumed include
// before it keeps the directive's own spelling from being reused, so a resume
// re-establishes the state, and it must start from "prior.c", not from the
// physical file name.
#line 10 "prior.c"
int x =
5
#line 124 "prior.c"
;
int y = __LINE__;
const char *f = __FILE__;
