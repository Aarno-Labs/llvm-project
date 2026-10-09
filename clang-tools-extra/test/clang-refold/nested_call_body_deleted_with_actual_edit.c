// RUN: %clang-refold-tester-verify-off nested_call_body_deleted_with_actual_edit
// A nested call is kept in a rebuilt formal only when its own rebuild is
// proven to expand to its new expansion.
//
// B replaces `("zero")`, the whole expansion of `PAREN("zero")`, with `"one"`,
// deleting the parentheses PAREN's body supplies.  Two rebuilds of CASE_'s
// second actual used to keep PAREN and were wrong:
//   * splicing `"one"` over the old expansion `("zero")` found inside the
//     formal spelling, which overlaps PAREN's own spelling, emitted
//     `PAREN"one"`, where PAREN is no longer invoked; and
//   * the child-expansion bridge, given PAREN rebuilt by projecting its
//     formal alone, emitted `PAREN("one")`, which expands to `("one")`.
// Neither PAREN's expansion nor its body survives, so the actual is replaced
// by B's `"one"`.  Verification is off so the planner alone must reach it.
#define PAREN(x) (x)
#define CASE_(n, s) \
  case n:           \
    fmt = s;        \
    break

const char *pick(int k) {
  const char *fmt = "";
  switch (k) {
    CASE_(0, PAREN("zero"));
    CASE_(1, PAREN("one"));
  }
  return fmt;
}
