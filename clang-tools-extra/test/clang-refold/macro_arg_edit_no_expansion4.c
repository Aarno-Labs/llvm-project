// RUN: %clang-refold-tester macro_arg_edit_no_expansion4
#define PRINT_FILE(FMT, X, Y) printf(FMT, X, Y)
PRINT_FILE("Error on file (%s) and line (%d)\n", "test.c", 2);
