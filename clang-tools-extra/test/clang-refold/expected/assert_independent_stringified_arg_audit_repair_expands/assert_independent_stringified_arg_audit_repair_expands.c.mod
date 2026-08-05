// RUN: %clang-refold-tester-relaxed-verify-repair assert_independent_stringified_arg_audit_repair_expands
//
// The repair counterpart of
// assert_independent_stringified_arg_relaxed_check_rejects: the same fold is
// unsound, but `--verify-output=repair` narrows instead of failing.
//
// The closing verification rejects the assembly, names the smallest region
// owning the divergence, and that region is expanded and the refold retried
// until the emitted source replays the edited stream.  The result gives up the
// `assert` callsite -- the expected .c.mod below carries the expansion -- which
// is the completeness that soundness costs here.
//
// Together the two tests pin both halves of the contract: unsound folds are
// never emitted, and the caller chooses between being told and being repaired.
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr))
int x = 5;
int y = 7;
int z = 9;
((x == z || x == y) ? (void)0 : __assert_fail("assertion failed"));
