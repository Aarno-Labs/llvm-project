// RUN: %clang-refold-tester-with-lines include_materialization_with_line_sensitive_file_suffix
#include "file_value.h"
const char *tail_file = __FILE__;
