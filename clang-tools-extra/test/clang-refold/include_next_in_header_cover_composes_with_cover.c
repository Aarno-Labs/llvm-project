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
// The composition law is a named discharge between two specific theorems, not
// a general rule about nested edits.  Source containment proves only that the
// two edits cannot both be applied, which is a statement about the applicator;
// it says nothing about what the surviving payload emits.  Nor is the cover's
// complete-source-closure authority enough on its own: that authority proves
// the cover accounted for every construct its range crossed, and re-emitting a
// construct's source accounts for it exactly as realizing its tokens does.
// Only the second discharges a rewrite obligation.
//
// So the two are told apart by evidence minted where the difference is known.
// The header planner hands its preserved-source pieces to the assembler when
// the closure capabilities are created, and every authorized construct outside
// that set is recorded on the edit as eliminated -- realized away rather than
// carried through.  Here the cover holds a source-closure capability over the
// exact `#include_next` construct [17,54) that the inner edit is authorized to
// rewrite, and names that construct as eliminated, so the rewritten directive
// is never emitted and the obligation to rewrite it is discharged.
//
// The inner edit's capability is moved onto the cover rather than deleted: the
// cover really does consume those protected bytes, so the emission audit must
// still find an authority for them.
//
// Every premise is load-bearing.  Suppressing just the elimination witness --
// having the planner decline to state its preserved pieces -- makes this test
// fail closed with an uncomposable edit set rather than silently keep working,
// and the witness is not vacuous: elsewhere in this suite a closure edit
// authorizes a construct it *preserves*, and that construct is correctly not
// recorded as eliminated.
//
// The rule stays narrow in three further ways: the inner edit must carry
// exactly one capability, the include-directive rewrite, over exactly its own
// range, so an edit that also does something else is never dropped; the
// construct is matched by exact identity rather than containment; and a
// zero-width insertion, or two edits claiming the same range, are still
// refused.
//
// The expected refold is the composition: the outer include replaced by the
// materialized body with the payload already in it, and the inner include gone
// because its entire contribution was deleted.
//
// This was the only shape in the 2088-cell A x B x C x D cross product that
// reached `UncomposableEmissionEditSet` for a reason other than the `#elifdef`
// arm-count gap pinned by `conditional_elifdef_arm_count_blocks_unrelated_edit`.
#include "incnext_cover_outer.h"
