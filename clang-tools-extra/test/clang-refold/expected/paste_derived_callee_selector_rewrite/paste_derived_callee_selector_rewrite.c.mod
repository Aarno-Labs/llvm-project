// RUN: %clang-refold-tester-with-lines paste_derived_callee_selector_rewrite
// Step 5 counterexample: DISPATCH derives the real callee name with ##.
// Editing ADD_ONE's body result from +1 to +2 is locally refoldable by
// changing the callee-selector argument ONE -> TWO and preserving the macro DAG.
#define CAT2(a, b) a##b
#define DISPATCH(NAME, X) CAT2(ADD_, NAME)(X)
#define ADD_ONE(x) ((x) + 1)
#define ADD_TWO(x) ((x) + 2)

int value = DISPATCH(TWO, 10);
