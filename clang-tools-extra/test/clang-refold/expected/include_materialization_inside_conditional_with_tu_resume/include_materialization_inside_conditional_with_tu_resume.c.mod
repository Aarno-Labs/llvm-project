// RUN: %clang-refold-tester-with-lines include_materialization_inside_conditional_with_tu_resume
#define ENABLE_HEADER 1
#define KEEP(x) ((x) + 1)
#if ENABLE_HEADER
#line 1 "headers/conditional_value.h"
int conditional_value = 2;
#line 6 "include_materialization_inside_conditional_with_tu_resume.c"
#endif
int after = KEEP(3);
