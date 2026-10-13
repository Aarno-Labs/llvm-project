// RUN: %clang-refold-tester-with-lines-verify-off macro_state_undef_before_line_observer_merged_line_payload
// Regression: B joins a `__LINE__` observer's line to the line before it, and
// the line-observer realization copies B's line, painted `X` included, to
// where `#define X (X + 1)` is live.  The realization carried no B-payload
// certificate, so the macro-state audit never read its tokens and `X`
// expanded again.  It now certifies the B tokens it copies, and the partition
// leaves `X` unbound for them.
int X = 1;
#define X (X + 1)
int r = X;
#undef X
int a = 1; int l = 12 + (X + 1);
#define X (X + 1)
int z = X;
