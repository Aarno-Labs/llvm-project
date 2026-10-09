// RUN: %clang-refold-tester-verify-off nested_identity_call_actual_comma_insertion
// An edit inside a nested call's actuals is proved by that call, not by
// finding its old expansion inside the enclosing formal's spelling.
//
// B appends `, idx = 0` to `"zero"`, the expansion of `ID("zero")`.  The
// args-only template solver found `"zero"` inside the formal spelling
// `ID("zero")` and spliced the new expansion there, emitting
// `CASE_(0, ID("zero", idx = 0))`: the comma gives ID a second actual, and the
// output no longer preprocesses.  The only arity check looked at CASE_'s own
// actual, where ID's parentheses hide the comma.  The edited case is expanded
// and the untouched one is kept.  Verification is off so the planner alone
// must refuse the splice.
//
// Reduced from bison's yysyntax_error in jq's parser.c, where
// `YYCASE_(0, YY_("syntax error"))` gained the same kind of comma operand.
#define ID(x) x
#define CASE_(n, s) \
  case n:           \
    fmt = s;        \
    break

const char *pick(int k) {
  const char *fmt = "";
  int idx = -1;
  switch (k) {
    CASE_(0, ID("zero"));
    CASE_(1, ID("one"));
  }
  return idx < 0 ? fmt : "";
}
