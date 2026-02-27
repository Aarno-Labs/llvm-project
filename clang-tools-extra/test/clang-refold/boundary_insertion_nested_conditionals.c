// RUN: %clang-refold-tester-with-lines boundary_insertion_nested_conditionals

// test84: nested conditionals.
#define OUT84 1
#define IN84  1
#if OUT84
  #if IN84
    int ARM84_START = 1;
  #else
    int ARM84_START = 2;
  #endif
#else
  int ARM84_START = 3;
#endif
int main(){ return ARM84_START; }
