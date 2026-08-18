// RUN: %clang-refold-tester-edit-map edit_map_sidecar_macro_argument_edit
//
// Coverage for the materialized-edit-map sidecar through the standard harness.
// The emission proofs in RefoldTextEditAssembler that record edit-map rows, and
// the ones that fail closed when a row has no deterministic B-side range, are
// all guarded on the sidecar having been requested, so a test that does not ask
// for it exercises none of them.  Asking for it here keeps that surface under
// the harness's own producer/output/--check comparisons rather than under a
// hand-rolled RUN sequence.
//
// The sidecar itself is not diffed: it records absolute paths, so pinning its
// bytes would be machine-dependent.
#define ADD(x, y) ((x) + (y))
int v = ADD(1, 2);
