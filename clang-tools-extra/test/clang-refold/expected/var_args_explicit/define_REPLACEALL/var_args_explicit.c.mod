// RUN: %clang-refold-tester var_args_explicit REPLACE
// RUN: %clang-refold-tester var_args_explicit REPLACEALL
// RUN: %clang-refold-tester var_args_explicit INSERT
// RUN: %clang-refold-tester var_args_explicit APPEND
// RUN: %clang-refold-tester var_args_explicit PREPEND
// RUN: %clang-refold-tester var_args_explicit REMOVE
// RUN: %clang-refold-tester var_args_explicit REMOVEHEAD
// RUN: %clang-refold-tester var_args_explicit REMOVETAIL
// RUN: %clang-refold-tester var_args_explicit REMOVEALL
#define PRINT(FMT, ARGS...) printf(FMT, ARGS)
PRINT("args: %s, %s, %s\n", 4, 5, 6);
