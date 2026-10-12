// RUN: %clang-refold-tester-verify-off payload_opening_call_after_expansion_output_is_undefined_and_restored
// Regression: B inserts `(2)` after the `F` that M's expansion ends with.  In
// B that `F` is an identifier, but realized as a TU insertion after `M(F)` the
// `(` would let F's live definition read the payload as its arguments.  The
// macro-name-in-B repair must see the call the payload opens: undefine `F`
// before the line, which keeps `M(F)` because no expansion on that line read
// `F`, and restore it for the later call.
int F(int);
#define F(v) v
#define M(a) (void)a
void g(void) { M(F); }
int h(void) { return F(5); }
