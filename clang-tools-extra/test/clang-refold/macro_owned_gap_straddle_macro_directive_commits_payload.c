// RUN: %clang-refold-tester macro_owned_gap_straddle_macro_directive_commits_payload
// Regression: a payload straddling a `#define` whose neighbouring material is
// produced by macro invocations rather than written in the translation unit.
//
// The identical straddle over the identical `#define` folds when `1,` and `2`
// are TU text; that is
// `structural_gap_straddle_composed_directive_kinds_commits_payload`.  Spelling
// them as invocations of `GAP_PRE` and `GAP_POST` used to surrender the whole
// translation unit at `obligation=OwnerClosedCover reason=NoOwnerClosedCover
// stage=classify`, with the trace naming what ran out --
//
//   macroOwnerExhausted=yes includeOwnerExhausted=yes mapsToTU=yes
//   TUByteSpan=none hasTUByteSpan=no
//
// -- so the payload mapped to the translation unit and yet had no byte span in
// it, because every A token it replaces came from an expansion.
//
// The two walks that turn a run of A tokens into a source span -- the
// structural tiler's run planner and `PlanTUByteSpan` -- each required every
// token to map strictly after the one before it.  Two tokens of one expansion
// map to the *same* bytes, the invocation's own spelling, so both walks read a
// whole invocation as a nonmonotone mapping.  A repeat is now admitted exactly
// when the producer's records prove the repeating group is one invocation's
// self-contained whole cover that the envelope wholly consumes, and the span
// then consumes that callsite spelling once.  `GAP_PRE` supplies two A tokens
// and `GAP_POST` one, so the hunk partitions into two macro-owned runs with the
// `#define` preserved between them.
//
// A partially consumed expansion still refuses: claiming the whole callsite
// spelling while replacing only some of what it produced would drop the tokens
// the hunk never asked to replace.
//
// Neither definition may be edited to realize a use-site payload, and this fold
// does not: it replaces the invocations, and both `#define`s survive.
#define GAP_PRE 1,
#define GAP_POST 2
int arr[] = { GAP_PRE
#define GAP_VALUE 3
GAP_POST };
