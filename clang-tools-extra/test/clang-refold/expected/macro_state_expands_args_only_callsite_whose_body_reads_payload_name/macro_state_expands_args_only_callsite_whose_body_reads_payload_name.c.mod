// RUN: %clang-refold-tester-verify-off macro_state_expands_args_only_callsite_whose_body_reads_payload_name
// Regression: B's actual names the enumerator `X`, while `M`'s body needs the
// macro `X`, so no macro state serves both inside one expansion.  The
// args-only `M(X + 1)` expanded both and emitted `5 + 1 + 5`.  Undefining `X`
// over the callsite is refused because the producer recorded `X` expanding
// there, the audit names `M`, and the fallback expands it under the partition.
enum { X = 3 };
#define X 5
#define M(a) int k = a + X;
#undef X
int k = X + 1 + 5;
