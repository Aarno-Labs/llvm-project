// RUN: %clang-refold-tester selector_tuple_ambiguous_replacement_callee_expands
// Fail-closed guard: a root the object-selector/tuple theorem ADMITS and cannot
// SOLVE must not be released to ordinary standard args-only replay.
//
// Neighbour of stress_selector_object_macro_callee_edit_selector_and_tuple.c.
// `APPLY(f, t)` is the `f t` selector/tuple forwarder, and the selector actual
// `OP` resolves through an object-like alias to the visible function-like
// `ADD`, so the object-selector/tuple theorem claims this root.  It then fails
// to solve it: B's `((10) - (3))` is reproduced by BOTH `SUB` and `MINUS` from
// the edited actuals, so no unique replacement callee is provable and the
// theorem declines.
//
// The edit reaches outside the tuple actual -- the operator changed as well as
// the operands -- so preserving the `OP` selector cannot realize B.  Ordinary
// args-only replay would nonetheless rewrite only the tuple actual and emit
// `APPLY(OP, (10, 3))`, which re-preprocesses to `((10) + (3))`; the closing
// verification catches that as `token mismatch at index 7: A='+' B='-'`.
// Refusing at the point of production instead yields the conservative
// expansion, per CLAUDE.md section 2.
//
// Scope of this test: it guards the fail-closed gate's EXISTENCE, and passes
// both before and after the admission/solving split -- the gate it replaced
// refused this root too.  What the split changed is *how* the domain is
// decided, not the verdict here.  Its companion,
// selector_tuple_contained_tuple_edit_still_folds.c, is the one that
// constrains the split itself.
//
// PINNED BY EXACT OUTPUT, NOT BY `%clang-refold-tester-expect-refold-fail`.
// The refold does not fail: whole-cover fallback materializes the expansion and
// clang-refold exits 0.  What must be pinned is that the emitted source is the
// expansion rather than a selector-preserving fold, so the assertion is on the
// output bytes.  Removing the fail-closed gate makes this test fail with a
// nonzero exit from the closing verification, not with an output diff.
#define OP ADD
#define APPLY(f, t) f t
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))
#define MINUS(a, b) ((a) - (b))

int x = ((10) - (3));
