// RUN: %clang-refold-tester macro_owned_gap_straddle_macro_directive_refuses
// XFAIL: *
// Refusal shape: a payload straddling a `#define` whose neighbouring material
// is produced by macro invocations rather than written in the translation unit.
//
// The identical straddle over the identical `#define` folds when `1,` and `2`
// are TU text.  Replacing them with invocations of `GAP_PRE` and `GAP_POST`
// refuses: `obligation=OwnerClosedCover reason=NoOwnerClosedCover
// stage=classify`, with the trace naming what ran out --
//
//   macroOwnerExhausted=yes includeOwnerExhausted=yes mapsToTU=yes
//   TUByteSpan=none hasTUByteSpan=no
//
// -- so the payload maps to the translation unit and yet has no byte span in
// it, because every A token it replaces came from an expansion.
//
// TRIAGE: incompleteness.  The macro-state proof that admits the TU-owned
// straddle is unchanged here; what is missing is a byte span to write the
// payload into once that proof has passed.  The invocations are whole and
// adjacent, so the span the fold needs is exactly the two invocations' source
// extent.
//
// Neither definition may be edited to realize a use-site payload, and the
// expected refold does not: it replaces the invocations, and both `#define`s
// survive.
#define GAP_PRE 1,
#define GAP_POST 2
int arr[] = { GAP_PRE
#define GAP_VALUE 3
GAP_POST };
