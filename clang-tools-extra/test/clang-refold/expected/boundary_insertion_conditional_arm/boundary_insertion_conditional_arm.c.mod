// RUN: %clang-refold-tester-with-lines boundary_insertion_conditional_arm

// test83: conditional arm boundary insertion.
#define FLAG83 1
#if FLAG83
int ARM83_START = 1;
#else
int ARM83_START = 2;
#endif
int INS83_ARM_BOUNDARY = ARM83_START + 1;
int main(){ return ARM83_START; }
