// RUN: %clang-refold-tester-with-lines macro_pragma_sideband_delete
#define EMIT_PRAGMA _Pragma("vendor alpha")
EMIT_PRAGMA
int value = 1;
