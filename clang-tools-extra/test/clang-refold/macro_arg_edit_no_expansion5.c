// RUN: %clang-refold-tester macro_arg_edit_no_expansion5
printf("file: %s, line: %d\n", __FILE_NAME__, __LINE__);
