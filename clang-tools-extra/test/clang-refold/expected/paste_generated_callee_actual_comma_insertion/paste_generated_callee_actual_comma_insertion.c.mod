// RUN: %clang-refold-tester-verify-off paste_generated_callee_actual_comma_insertion
// An old expansion found inside a parenthesis of the formal spelling is not
// replaced in place, even when no recorded invocation spells that parenthesis.
//
// `CAT(I, D)("zero")` expands to `"zero"` because the pasted name `ID` then
// collects `("zero")` as its actuals.  The producer records no spelling for a
// callee formed by pasting, so no child invocation overlaps `"zero"`, but the
// parenthesis around it shows that some invocation consumed it.  Splicing B's
// `"zero", idx = 0` there would give ID two actuals.  The edited case is
// expanded and the untouched one is kept.  Verification is off so the planner
// alone must refuse the splice.
#define CAT(a, b) a##b
#define ID(x) x
#define CASE_(n, s) \
  case n:           \
    fmt = s;        \
    break

const char *pick(int k) {
  const char *fmt = "";
  int idx = -1;
  switch (k) {
    case 0: fmt = "zero", idx = 0; break;
    CASE_(1, CAT(I, D)("one"));
  }
  return idx < 0 ? fmt : "";
}
