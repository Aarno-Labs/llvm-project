// RUN: %clang-refold-tester pragma_operator_diagnostic_folds_back
// Editing the warning name in a _Pragma(GCC diagnostic ...) folds back into
// the _Pragma operator.
_Pragma("GCC diagnostic ignored \"-Wall\"")
int a = 1;
