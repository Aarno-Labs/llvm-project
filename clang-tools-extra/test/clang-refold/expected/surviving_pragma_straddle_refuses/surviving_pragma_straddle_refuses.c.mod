// RUN: %clang-refold-tester surviving_pragma_straddle_refuses
// XFAIL: *
// Refusal shape: one B token replacing material on both sides of a pragma
// that survives into the preprocessed stream.
//
// This is the straddling payload of the `#pragma region` refusal test, over a
// pragma clang re-emits rather than one it consumes.  It used to refuse two
// stages earlier than that test, at
// `obligation=ProducerFactsAvailable reason=MissingProducerFacts stage=tok`,
// because the straddle unmapped the directive's B-side neighbour and the
// sideband normalizer could not recognize the two copies as one directive.
// That was incompleteness in the pairing and is fixed: the certified-anchor
// window pairs them, the copies leave the token stream, and the source pragma
// is preserved untouched.  See
// `surviving_pragma_adjacent_following_edit_preserves_directive`.
//
// What is left is the refusal this cell was really about, and it now reports
// itself as such:
//
//   obligation=OwnerClosedCover reason=NoOwnerClosedCover stage=classify
//   hunk=0 aTokens=[6,9) bTokens=[6,7)
//
// The payload replaces `1 , 2`, which spans the directive, so nothing fixes
// which side of it the single B token `9` belongs on.  `pack` is unclassified,
// so the taxonomy treats it as binding whatever follows and as changing state
// the payload may observe; both placements replay B while differing in what
// they mean.  This is the same undetermined placement that
// `undetermined_payload_side_refuses_without_dropping_structure` pins over a
// consumed pragma, reached here over a re-emitted one.
//
// TRIAGE: correct for now, and the honest bucket for it is the placement
// question rather than the pairing one.  It is a tripwire: if this starts
// passing, a payload has been committed to one side of a protected structure
// without proving which side it belongs on.
//
// The expected refold records the payload-after-directive placement.  It is a
// fold of this input, not a proof that this input has only one.
int arr[] = {
#pragma pack(1)
9 };
