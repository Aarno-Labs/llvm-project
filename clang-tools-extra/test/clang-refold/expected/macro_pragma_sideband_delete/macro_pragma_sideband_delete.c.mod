// RUN: %clang-refold-tester-with-lines macro_pragma_sideband_delete
#define EMIT_PRAGMA _Pragma("vendor alpha")
#line 4 "macro_pragma_sideband_delete.c"
int value = 2;
