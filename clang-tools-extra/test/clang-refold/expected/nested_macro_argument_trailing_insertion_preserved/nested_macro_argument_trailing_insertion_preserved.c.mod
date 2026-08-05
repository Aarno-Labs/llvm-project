// RUN: %clang-refold-tester nested_macro_argument_trailing_insertion_preserved
// An edit that appends a token at the trailing edge of a macro argument must
// survive when that invocation is nested inside another one.  The argument's
// neighbouring A material is the outer macro's body text, fixed by the
// `#define`, so the argument is the only surface that can materialize the
// appended `]`.  DAG lifting used to trim boundary insertions here and dropped
// it silently -- both the replay and expected ledgers were computed through the
// same trimmed mapper, so they agreed while both losing the token, and the
// emitted source carried an unbalanced `[`.
#define SHL16(a,shift) (a)
#define SUB16(a,b) ((a)-(b))

short f(short *x, short *cdbk, int i, int j)
{
   short tmp;
   tmp = SUB16(x[j], SHL16((short)cdbk[i++], 5));
   return tmp;
}
