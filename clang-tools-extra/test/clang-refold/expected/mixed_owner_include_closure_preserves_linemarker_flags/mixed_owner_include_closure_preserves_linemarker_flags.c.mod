// RUN: %clang-refold-tester-with-lines mixed_owner_include_closure_preserves_linemarker_flags
int x =
3
# 123 "gap.c" 1

;
int y = __LINE__;
