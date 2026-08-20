// RUN: %clang-refold-tester-expect-refold-fail surviving_pragma_straddle_refuses
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
// `consumed_pragma_straddle_commits_payload_and_preserves_directive` pins over a
// consumed pragma, reached here over a re-emitted one.
//
// PINNED BY ASSERTION, NOT BY `XFAIL`.  The harness requires the refold to
// fail, so this reports as a passing test.  A shape that must never fold is not
// unfinished work, and an expected-failure entry would say it is -- and would
// sit in the remaining-work count forever.  If the shape ever starts folding
// this test fails, which is the same alarm `XPASS` used to raise.  The expected
// refold beside this file is no longer diffed; it stays as the record of what
// admitting it would have produced.  The `.c.i` still is, so producer drift is
// still caught.
//
// TRIAGE: correct for now, and the honest bucket for it is the placement
// question rather than the pairing one.  It is a tripwire: if this starts
// passing, a payload has been committed to one side of a protected structure
// without proving which side it belongs on.
//
// The expected refold records the payload-after-directive placement.  It is a
// fold of this input, not a proof that this input has only one.
int arr[] = { 1,
#pragma pack(1)
2 };
