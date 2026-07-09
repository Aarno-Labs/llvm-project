// RUN: %clang-refold-tester pragma_operator_from_macro_arg_folds_back
// XFAIL: *
// Remaining gap (not a miscompile): a _Pragma produced from a macro argument
// (_Pragma(#x)) now folds to a raw #pragma at the operator site, but not back
// into the feeding macro argument DIAG(message("bye")); that would require
// args-only inversion through the macro. Direct _Pragma content edits fold back
// into the operator (see pragma_operator_message_content_folds_back).
#define DIAG(x) _Pragma(#x)
DIAG(message("hi"))
int a = 1;
