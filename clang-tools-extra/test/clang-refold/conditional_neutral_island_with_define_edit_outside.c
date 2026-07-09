// RUN: %clang-refold-tester conditional_neutral_island_with_define_edit_outside DEF
#if defined(DEF)
#define GREETING 1
int g = GREETING;
#endif
int after = 7;
