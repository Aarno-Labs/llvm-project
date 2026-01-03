// RUN: %clang-refold-tester global_obj_like_macros_in_macros
#define PRINT_FILE(FMT) printf(FMT, __FILE_NAME__, __LINE__)
PRINT_FILE("Error on file (%s) and line (%d)\n");
