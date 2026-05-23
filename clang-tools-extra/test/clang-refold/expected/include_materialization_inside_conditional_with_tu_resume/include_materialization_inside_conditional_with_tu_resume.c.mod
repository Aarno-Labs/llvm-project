// RUN: %clang-refold-tester-with-lines include_materialization_inside_conditional_with_tu_resume
#define ENABLE_HEADER 1
#define KEEP(x) ((x) + 1)
#if ENABLE_HEADER
int conditional_value = 2;
#endif
int after = KEEP(3);
