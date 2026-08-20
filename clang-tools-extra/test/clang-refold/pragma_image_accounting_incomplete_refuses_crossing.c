// RUN: %clang-refold-tester-relaxed-expect-refold-fail pragma_image_accounting_incomplete_refuses_crossing
// A pragma crossing that is otherwise proven refuses when the producer could
// not account for every directive it printed.
//
// Crossing a preserved pragma requires that the preprocessor emitted nothing
// for it, which the producer records by giving each re-emitted directive its
// image in A.  Absence of an image is therefore the *admitting* answer, and an
// admitting answer may not rest on missing data: a directive that was printed
// but that no callback could bind to an item would look exactly like one that
// was consumed, and a payload would be committed across a directive that is
// spelled into both streams.
//
// That gap is real, not hypothetical.  Clang calls
// `PPCallbacks::PragmaAssumeNonNullEnd` with an invalid `SourceLocation`
// (`clang/lib/Lex/Pragma.cpp`), so the `end` directive below is printed while
// nothing identifies which pragma item it belongs to.  The producer counts what
// it prints and compares that against the items carrying an image, and reports
// the mismatch as `pragma_images_complete: false`.
//
// The first pragma here is `region`, whose crossing is proven in
// `consumed_pragma_straddle_commits_payload_and_preserves_directive` on the
// same edit.  The only difference is the unattributable emission later in the
// file, and that is enough to take the whole translation unit to the
// fail-closed answer:
//
//   structure=Pragma ... crossable=false reason=PragmaNotConsumed
//
// TRIAGE: correct, and the refusal is the assertion.  If this starts folding,
// absence of a recorded image is being read as proof of consumption without the
// certificate that makes it one.
int arr[] = { 1,
#pragma region
2 };
#pragma clang assume_nonnull begin
int guarded;
#pragma clang assume_nonnull end
