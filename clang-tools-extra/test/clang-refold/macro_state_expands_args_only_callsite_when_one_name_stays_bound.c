// RUN: %clang-refold-tester-verify-off macro_state_expands_args_only_callsite_when_one_name_stays_bound
// Regression: B's actual names the enumerators `X` and `Z`.  `X` can be
// undefined over the args-only `M(X + Z + 1)`, but `M`'s body needs the macro
// `Z`, so `Z` cannot.  Repairing `X` restaged the edit, and the audit then
// skipped it, so `Z` expanded to 6 in the actual.  The audit now keeps a
// per-edit record of what each repair unbound, and expands `M` instead.
enum { X = 3, Z = 4 };
#define X 5
#define Z 6
#define M(a) int k = a + Z;
M(1)
