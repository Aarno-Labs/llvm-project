// RUN: %clang-refold-tester-verify-off macro_state_repairs_deletion_joining_function_like_name_to_paren
// Regression: B deletes `*p,` from `F *p, (y);`, where `F` is a typedef and a
// function-like macro.  B shows `F (y)` with `F` an identifier, but the
// deletion's certificate held no B token, so nothing read the seam it makes
// between `F` and `(`, and the output called `F`.  An empty B range now
// marks where a deletion joins the tokens on either side of it.
typedef int F;
#define F(v) v
F *p, (y);
int z = F(3);
