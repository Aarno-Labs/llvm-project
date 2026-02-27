// RUN: %clang-refold-tester-with-lines nested_invocations_arg_parsing_nested_parens_commas

// test96.c: nested invocations; arg parsing through nested parens/comma.
#define F(x) x
#define G(a,b) ((a) + (b))
#define H(x) (F(x) * 2)

int main(void) {
  int x = H(G(1, (2 + 3)));
  return x == 12 ? 0 : 1;
}
