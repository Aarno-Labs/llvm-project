// RUN: %clang-refold-tester-with-lines unknown_pragma_preserved_in_pp_output
#pragma clang_tutorial force_define 42
int x = PRAGMA_TRIGGERED;
