// RUN: %clang-refold-tester pragma_operator_pack_value_folds_back
// Editing the pack value of a _Pragma folds back into the _Pragma operator.
_Pragma("pack(2)")
int a = 1;
_Pragma("pack()")
int b = 2;
