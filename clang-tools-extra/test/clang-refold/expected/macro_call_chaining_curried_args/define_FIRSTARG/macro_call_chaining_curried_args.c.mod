// RUN: %clang-refold-tester macro_call_chaining_curried_args FIRSTARG
// RUN: %clang-refold-tester macro_call_chaining_curried_args SECONDARG
// RUN: %clang-refold-tester macro_call_chaining_curried_args BOTHARGS
// RUN: %clang-refold-tester macro_call_chaining_curried_args BODY
// Layer 1: Selection
#define CHOOSE_ADD()    SUM_PROT
#define CHOOSE_SUB()    DIFF_PROT

// Layer 2: Intermediate "Protector" 
// This resolves to the actual operation macro
#define SUM_PROT(x)     SUM_OP(x)
#define DIFF_PROT(x)    DIFF_OP(x)

// Layer 3: Final Operations
#define SUM_OP(x) (y)   ((x) + (y))
#define DIFF_OP(x) (y)  ((x) - (y))

/*
   Wait! The standard C preprocessor doesn't support currying like (x)(y) 
   directly in one definition. To get PICK()(10)(20) to work, the first 
   expansion must result in something that is *itself* a function-like macro.
*/

// --- THE CORRECT MULTI-LAYER APPROACH ---

#define GET_MATH(type)  DO_##type

#define DO_ADD(x)       ADD_STAGE2(x)
#define ADD_STAGE2(x)(y) ((x) + (y))

#define DO_MUL(x)       MUL_STAGE2(x)
#define MUL_STAGE2(x)(y) ((x) * (y))

int main(void) {
  // 1. GET_MATH(ADD) expands to DO_ADD
  // 2. DO_ADD(10) expands to ADD_STAGE2(10)
  // 3. ADD_STAGE2(10)(20) expands to ((10) + (20))
  int a = GET_MATH(ADD)(11)(20); 
    
  // 1. GET_MATH(MUL) expands to DO_MUL
  // 2. DO_MUL(5) expands to MUL_STAGE2(5)
  // 3. MUL_STAGE2(5)(4) expands to ((5) * (4))
  int b = GET_MATH(MUL)(5)(4);

  return (a == 30 && b == 20) ? 0 : 1;
}
