// RUN: %clang-refold-tester-clang-flags include_next_in_header_cover_edit_set_uncomposable -- -I headers/t_incnext_cover/a -I headers/t_incnext_cover/b
// XFAIL: *
// Refusal shape: a replacement covering an `#include_next` together with the
// material on both sides of it, where the whole construct is owned by a header
// rather than by the translation unit.
//
// The identical edit over the identical `#include_next` folds when the array
// and the directive are written in the translation unit.  Moving them into a
// header makes the edit uncomposable at emission:
//
//   obligation=EmissionEditSetComposable reason=UncomposableEmissionEditSet
//   stage=edits/apply
//
// Two edits are in play and they overlap: the outer header is materialized into
// the translation unit, and the inner `#include_next`'s contribution is deleted
// inside the text being materialized.  Neither is wrong on its own; the pair
// cannot be applied in either order.
//
// TRIAGE: incompleteness.  Both edits are proven -- the refusal is not that
// either realization is unsound but that the emitter has no composition for a
// deletion nested inside a materialization it is producing.  The expected
// refold is the composition: the outer include replaced by the materialized
// body with the payload already in it, and the inner include gone because its
// entire contribution was deleted.
//
// This is the only shape in the 2088-cell A x B x C x D cross product that
// reaches `UncomposableEmissionEditSet` for a reason other than the
// `#elifdef` arm-count gap pinned by
// `conditional_elifdef_arm_count_blocks_unrelated_edit`.
int arr[] = { 9 };
