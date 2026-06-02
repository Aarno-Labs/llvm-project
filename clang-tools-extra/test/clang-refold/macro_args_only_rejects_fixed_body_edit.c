// RUN: %clang-refold-tester-with-lines macro_args_only_rejects_fixed_body_edit
#define A_MAC_CALL(F, X) a_bias_call((F), (X))
int d = A_MAC_CALL(&target, b);
