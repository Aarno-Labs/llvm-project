// RUN: %clang-refold-tester-verify-off materialized_header_macro_state_brackets_definition_bound_in_tu
// Regression: the header's callsite is realized from B as `F (2) != 0`, under
// the function-like `F` the translation unit defines before the `#include`.
// The materialized header only considered definitions spelled in the header,
// so `F (2)` was called.  The definition bound at the invocation is now found
// by processing order wherever it was spelled, and bracketed in the header.
int F(int v);
#define F(v) v
#include "materialized_header_macro_state_brackets_definition_bound_in_tu.h"
int j = F(5);
