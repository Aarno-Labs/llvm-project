// RUN: %clang-refold-tester selector_tuple_contained_tuple_edit_still_folds
// Completeness guard for the object-selector/tuple admission/solving split.
//
// The companion to selector_tuple_ambiguous_replacement_callee_expands.c, and
// the case that separates "the theorem owns this root" from "ordinary replay
// must not take it."  Both share the shape: `APPLY(f, t)` is the `f t`
// forwarder, `OP` resolves through an object-like alias to the visible
// function-like `ADD`, and `PLUS` carries a byte-identical replacement list, so
// B is reproduced by two candidate callees and the object-selector/tuple
// theorem declines for want of a unique one.  The theorem therefore ADMITS this
// root and fails to SOLVE it, exactly as in the companion test.
//
// The difference is where the edit lands.  Here only the operands changed --
// `(9, 4)` to `(10, 3)` -- so every hunk in the whole cover lies inside the
// tuple formal that ordinary standard args-only replay is able to rewrite.  The
// old `OP` selector is still correct, and `APPLY(OP, (10, 3))` re-preprocesses
// to exactly B.  Preserving the selector abstraction is both sound and the
// tighter result, so args-only must still be allowed to take this case.
//
// Scope: this test passes against the pre-split implementation too, whose
// fail-closed gate carried the same containment condition under a separately
// coded domain check.  It is coverage of behavior the split preserves, not
// evidence for the split.  What it earns its place for is pinning the carve-out
// against a blanket "admitted then unsolved always fails closed" rule.  That rule is sound but strictly over-refuses: it would
// emit the expansion `int x = ((10) + (3));` here, losing a correct fold for no
// soundness gain.  This test fails -- by output diff, not by exit code -- if
// the containment condition in the fail-closed gate is dropped.
#define OP ADD
#define APPLY(f, t) f t
#define ADD(a, b) ((a) + (b))
#define PLUS(a, b) ((a) + (b))

int x = APPLY(OP, (10, 3));
