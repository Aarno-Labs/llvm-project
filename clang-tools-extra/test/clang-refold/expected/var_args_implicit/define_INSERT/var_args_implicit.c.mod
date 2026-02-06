// RUN: %clang-refold-tester var_args_implicit REPLACE
// RUN: %clang-refold-tester var_args_implicit REPLACEALL
// RUN: %clang-refold-tester var_args_implicit INSERT
// RUN: %clang-refold-tester var_args_implicit APPEND
// RUN: %clang-refold-tester var_args_implicit PREPEND
// RUN: %clang-refold-tester var_args_implicit REMOVE
// RUN: %clang-refold-tester var_args_implicit REMOVEHEAD
// RUN: %clang-refold-tester var_args_implicit REMOVETAIL
// RUN: %clang-refold-tester var_args_implicit REMOVEALL
#define PRINT(FMT, ...) printf(FMT, __VA_ARGS__)
PRINT("args: %s, %s, %s\n", 1, "foo", "bar", 2, 3);
