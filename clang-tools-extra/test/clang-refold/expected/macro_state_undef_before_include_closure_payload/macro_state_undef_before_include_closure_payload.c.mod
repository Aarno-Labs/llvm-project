// RUN: %clang-refold-tester-verify-off macro_state_undef_before_include_closure_payload
// Regression: B joins the header's line to the line after the include, so a
// TU include closure realizes the header's tokens from B, painted `X`
// included, where `#define X (X + 1)` is live.  The closure carried no
// B-payload certificate, so the macro-state audit never read its tokens and
// `X` expanded again.  The closure now certifies the B material it copies,
// and the partition leaves `X` unbound for it.
int X = 1;
#define X (X + 1)
int r = X;
#undef X
int h = (X + 1) + 5;
