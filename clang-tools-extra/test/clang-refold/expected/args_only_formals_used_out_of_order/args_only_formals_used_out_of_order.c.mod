// RUN: %clang-refold-tester-verify-off args_only_formals_used_out_of_order
// Args-only replay must take each occurrence's formal from the producer, not
// from the order occurrences appear in the expansion.
//
// `SWAP` emits `b` before `a`.  Numbering the occurrences by position named the
// edited `1` formal `b`, so no args-only rewrite matched B and the invocation
// was expanded.  The producer's formal index makes the edit `a`, and the
// invocation folds to `SWAP(5, 2)`.
#define SWAP(a, b) b - a
int r = SWAP(5, 2);
