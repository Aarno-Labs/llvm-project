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
// TRIAGE: incompleteness, and there are TWO blockers, not one.  The second is
// reachable only once the first is fixed, so the diagnosis above stops at the
// first.
//
//   1. Byte composition.  Both edits are proven -- the refusal is not that
//      either realization is unsound but that the emitter has no composition
//      for a deletion nested inside a materialization it is producing.  The
//      inner edit is not an overlap to be ordered, it is *subsumed*: the cover
//      replaces every byte it would rewrite, so dropping it cannot change an
//      emitted byte.  Adding that law to the byte applicator does clear this
//      refusal.
//
//   2. The include realization envelope, which the first blocker was hiding.
//      With the crossing composed, the run reaches emission and refuses again,
//      one stage later:
//
//        ordinary edit [1627,1660) overlaps protected Include interval
//        [1627,1660) without exact specialized authority
//
//      That edit deletes the outer `#include` and carries no accepted-result
//      carrier and no authority at all.  Upstream,
//      `ResolveIncludeRealizationBTokenEnvelope` produced nothing for the
//      outer include -- the owner realization is accepted with `A=[0,13)
//      B=[0,0)` and `includeEnvelopeEvidence=Unknown`, and its witness key
//      records `b_tokens:[0,0)` -- although every B token in the unit comes
//      from inside that header.  So the materialization stages an empty edit
//      at the directive site and the emission firewall correctly refuses it.
//
// The expected refold is still the composition: the outer include replaced by
// the materialized body with the payload already in it, and the inner include
// gone because its entire contribution was deleted.  Reaching it needs the
// envelope resolved for an include whose cover is the whole translation unit,
// not only the byte-composition law.
//
// This is the only shape in the 2088-cell A x B x C x D cross product that
// reaches `UncomposableEmissionEditSet` for a reason other than the
// `#elifdef` arm-count gap pinned by
// `conditional_elifdef_arm_count_blocks_unrelated_edit`.
#include "incnext_cover_outer.h"
