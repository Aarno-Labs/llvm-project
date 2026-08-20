// RUN: %clang-refold-tester-relaxed consumed_pragma_straddle_commits_payload_and_preserves_directive
// One B token replacing material on both sides of a pragma the preprocessor
// consumes commits to a side and keeps the directive.
//
// The payload's side is not fixed by alignment: `#pragma region` produces no
// tokens, so `9` replaying after it and `9` replaying before it are the same
// preprocessed stream.  That is only admissible when the two placements also
// mean the same thing, which needs both halves of the pragma question answered.
//
//   * The preprocessor emits nothing for this spelling.  That is a producer
//     fact -- the pragma has no recorded image in A -- and not a guess from the
//     directive text.  A pragma clang re-emits is spelled into both streams and
//     the payload's side of it *is* token order; see
//     `surviving_pragma_straddle_refuses`, which must keep refusing.
//   * `region` changes no state a payload could observe and binds to nothing
//     that follows it.  Clang registers `PragmaRegionHandler` for `region` and
//     `endregion` with an empty handler body, documented as an editor-only
//     pragma, so the classification is read off Clang's own handler rather than
//     assumed.
//
// Both answers are required and neither implies the other, which is why this
// cell refused for so long: the crossing used to gate on the state effect as a
// stand-in for the emission question, and an unclassified spelling therefore
// failed the wrong test.
//
// The directive survives in the refolded source.  A refusal here would have
// cost the file: emitting the edited preprocessed stream also replays B -- it
// is B -- while deleting this comment and the pragma with it, which is the one
// result the refolder may never produce.
int arr[] = { 
#pragma region
9 };
