// RUN: %clang-refold-tester-with-lines boundary_insertion_macro_generated_predicate

// test85: conditional uses macro-generated predicate.
#define F85(x) ((x) == 1)
#if F85(1)
int ARM85_START = 1;
#else
int ARM85_START = 2;
#endif
int INS85_ARM_BOUNDARY = ARM85_START + 1;
int main(){ return ARM85_START; }
