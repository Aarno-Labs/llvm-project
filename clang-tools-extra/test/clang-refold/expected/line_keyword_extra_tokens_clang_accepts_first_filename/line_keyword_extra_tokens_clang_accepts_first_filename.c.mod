// RUN: %clang-refold-tester-with-lines line_keyword_extra_tokens_clang_accepts_first_filename
#line 10 "prior_logical.c"
#line 100 "first_logical.c" "ignored.c"
#line 101 "first_logical.c"
int keep = __LINE__;
const char *file = __FILE__;
