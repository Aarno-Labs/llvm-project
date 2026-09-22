// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_va_opt_line_macro_gap
#define LINE_NO(...) __VA_OPT__(123)
int x =
3
#line LINE_NO(foo) "gap.c"

;
int y = __LINE__;
