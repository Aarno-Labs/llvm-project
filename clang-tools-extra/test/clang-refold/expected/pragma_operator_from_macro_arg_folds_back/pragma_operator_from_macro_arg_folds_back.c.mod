// RUN: %clang-refold-tester pragma_operator_from_macro_arg_folds_back
// XFAIL: *
// Gap (not a miscompile; --check passes): editing the pragma text produced by _Pragma(#x) should refold back into the macro argument that feeds it.
#define DIAG(x) _Pragma(#x)
DIAG(message("bye"))
int a = 1;
