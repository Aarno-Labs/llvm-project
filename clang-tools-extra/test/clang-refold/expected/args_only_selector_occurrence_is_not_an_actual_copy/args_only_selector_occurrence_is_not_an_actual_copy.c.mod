// RUN: %clang-refold-tester-verify-off args_only_selector_occurrence_is_not_an_actual_copy
// An occurrence of a formal that holds the callee it selects is not a copy of
// the actual and cannot be inverted from B's spelling.
//
// `f` names the callee in `f(0)`, and the producer attributes `G`'s output `g`
// to `f`.  B spells `h` there; reading that as the new actual emits
// `CALL(h)`, which expands to `h(0) + 1` instead of `h + 1`.  The fold is
// refused and the invocation is expanded.
static int g = 1, h = 2;
#define G(x) g
#define CALL(f) f(0) + 1
int r = h + 1;
