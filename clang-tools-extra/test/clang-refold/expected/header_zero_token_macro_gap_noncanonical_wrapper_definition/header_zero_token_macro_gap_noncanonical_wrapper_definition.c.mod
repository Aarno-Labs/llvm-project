// RUN: %clang-refold-tester-verify-off header_zero_token_macro_gap_noncanonical_wrapper_definition
//
// Regression: a zero-token wrapper call in a rewritten header gap, whose
// definition is continued onto a second line, is proved neutral.  The proof
// used to read the replacement list out of the directive's `text`, which is a
// canonical re-rendering: every offset past the continuation was shifted, the
// nested EMPTY() call was not found, and the header fallback kept A's `int` in
// front of `after`.  The proof now walks the producer's source range for each
// replacement-list token.
#define KEEP(x) ((x) + 1)
#define EMPTY()
#define WRAP_EMPTY() \
  EMPTY()

int untouched = KEEP(5);

int before = 10,
WRAP_EMPTY()
 after = 20;
