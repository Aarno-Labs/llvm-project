// RUN: %clang-refold-tester-verify-off payload_opening_call_after_spelled_name_is_undefined
// Regression: B inserts `(2)` after a spelled `F` that names a live
// function-like macro.  B keeps `F` an identifier, so the payload's `(` must
// not become F's argument list: the repair undefines `F` before the line,
// whose spelled `F` was never expanded.  A text scan of that line would see
// `F (2)` and refuse; the producer's records say nothing on it expanded `F`.
int F(int);
#define F(v) v
#undef F
void g(void) { (void)F (2); }
