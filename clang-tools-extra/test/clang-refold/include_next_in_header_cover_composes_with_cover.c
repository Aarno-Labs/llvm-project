// RUN: %clang-refold-tester-clang-flags include_next_in_header_cover_composes_with_cover -- -I headers/t_incnext_cover/a -I headers/t_incnext_cover/b
// Regression: an edit covering an `#include_next` together with the material
// on both sides of it composes, even when the whole construct is owned by a
// header rather than by the translation unit.
//
// One hunk spans the include boundary: `1,` and `2` are outer-header bytes and
// `0,` comes from the body, and B replaces all of it with `9`.  Two edits are
// therefore staged in the outer header's byte space and they overlap -- the
// cover edit `[14,55)` realizing the whole run from B, and the inner include's
// own directive rewrite `[17,54)` inside it.
//
// Neither is wrong, and no order applies both, because the inner edit's bytes
// are not in the output at all: the cover replaced them.  The applicator used
// to see only "overlapping normalized edits" and refuse the translation unit.
//
// The composition law is subsumption rather than ordering.  An edit whose
// source range another edit wholly replaces contributes nothing to the emitted
// bytes, so removing it cannot change a single one -- which is what makes this
// a law and not a preference between two proven edits.  It is kept narrow:
// containment must be strict on at least one side, so two edits claiming the
// same range stay a genuine ambiguity and are still refused; and both must
// replace bytes, so a zero-width insertion is never dropped on the grounds
// that something replaced the bytes it does not own.
//
// The expected refold is the composition: the outer include replaced by the
// materialized body with the payload already in it, and the inner include gone
// because its entire contribution was deleted.
//
// This was the only shape in the 2088-cell A x B x C x D cross product that
// reached `UncomposableEmissionEditSet` for a reason other than the `#elifdef`
// arm-count gap pinned by `conditional_elifdef_arm_count_blocks_unrelated_edit`.
#include "incnext_cover_outer.h"
