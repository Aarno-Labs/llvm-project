// RUN: %clang-refold-tester-verify-off include_closure_relocated_zero_token_callsite_keeps_consumed_define
// Regression: the first edit consumes `#define EMPTY()`.  A TU include closure
// joins the header's line to a later one and keeps the zero-token `EMPTY()`
// it spans by re-emitting it after its B material.  That callsite counted as
// consumed by the closure, so nothing kept the definition and `EMPTY ( )`
// reached the output.  A callsite an edit re-spells now survives, and the
// definition is kept for it.
int a1[] = { 11,
#define EMPTY()
22 };
#include "include_closure_plain_value.h"
EMPTY()
int t = 5;
int z = 4;
