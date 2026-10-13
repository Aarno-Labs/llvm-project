// RUN: %clang-refold-tester-verify-off split_expansion_single_token_fragment_composes_with_neighbour
// Regression: the token diff gives one hunk `?` and the first `(` of the
// second `NUL`'s expansion `((void *)0)`, keeping the rest of it as stable
// tokens.  The hunk's direct edit replaces the whole `NUL` callsite, so it
// dropped `(void *)0)` from the output.  An edit whose source spells tokens
// outside its hunk is now a fragment: it composes with the neighbouring hunk
// into an edit realizing the whole expansion, or the unit fails closed.
#define NUL ((void *)0)
char *f(char *cursor, char *separator, int i) {
  cursor = separator == NUL ? NUL : separator + 1;
  return cursor;
}
