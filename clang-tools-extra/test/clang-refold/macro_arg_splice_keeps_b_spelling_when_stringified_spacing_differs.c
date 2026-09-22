// RUN: %clang-refold-tester-verify-off macro_arg_splice_keeps_b_spelling_when_stringified_spacing_differs
//
// An args-only rewrite keeps A's spelling of the argument tokens B left
// unchanged.  Here that would give `S(a-b)`: the same tokens as B's `a - b`,
// but `#x` spells them "a-b", not the "a - b" B has.  The splice is refused
// and the argument is spelled from B.
#define S(x) #x
const char *s = S(a+b);
