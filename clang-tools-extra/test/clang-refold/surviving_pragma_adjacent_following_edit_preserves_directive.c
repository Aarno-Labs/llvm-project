// RUN: %clang-refold-tester surviving_pragma_adjacent_following_edit_preserves_directive
// An edit whose first replaced token is the immediate successor of a pragma
// that survives into the preprocessed stream keeps the directive and folds.
//
// A pragma clang re-emits occupies bytes in A that the producer's token records
// do not cover, so the sideband normalizer has to recognize the A and B copies
// as one directive and remove both from the token stream before the structural
// diff runs.  It used to do that by projecting the B normal-token gap back
// through the A/B map and requiring the projection to name one exact A gap,
// which needs the flanking A tokens to be mapped *and adjacent*.  Replacing the
// token right after the directive unmaps exactly that neighbour, so the
// projection refused, both copies stayed in the token stream, and the A-stream
// token-count check then routed the whole translation unit to the terminal
// fallback -- losing every comment and directive in it over one edited
// neighbour.
//
// Identity does not need a gap.  Two directives are the same occurrence when
// they sit between the same alignment-certified anchors, and that window is
// computed the same way from either stream, so it survives an edit that moves
// the absolute gaps.  Here the anchors `int a = 1 +` are matched on both sides
// and the directive is after all five of them in A and in B; the replaced `2`
// is not an anchor and does not enter the count.
//
// An exact gap is still required where nothing else can supply a placement: a
// B-only sideband insertion has no A-side line to inherit one from.  See
// `surviving_pragma_straddle_refuses` for the payload whose side of the
// directive is genuinely undetermined -- it pairs now, and refuses one stage
// later for that reason instead.
//
// Applies to every pragma clang re-emits -- `pack`, `message`,
// `GCC diagnostic`, and the `_Pragma` spellings of each -- and to no pragma it
// consumes.
int a = 1 +
#pragma pack(1)
2;
