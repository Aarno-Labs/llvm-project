// RUN: %clang-refold-tester macro_owned_gap_edit_before_surviving_pragma_emits_directive_once
// A macro-owned edit that ends where a surviving pragma begins realizes its
// B material from the bytes its own tokens occupy, so the directive is emitted
// exactly once.
//
// `#pragma pack(1)` is re-emitted into the preprocessed stream, so it appears
// in both A and B.  Sideband normalization pairs the two copies and removes
// their tokens from the streams fed to the structural diff, but deliberately
// leaves the bytes in the buffers; the source directive is preserved as-is and
// no sideband source edit is produced.  The remaining edit is `1` -> `5`, owned
// by `GAP2_PRE`, realized by whole-cover replacement of the invocation.
//
// The whole cover is B tokens `5` `,`.  Reading its replacement text as the
// bytes from the first token up to the *next* token's first byte runs past the
// filtered directive line, so the realized text was `5,\n#pragma pack(1)` and
// the assembly carried the directive twice -- once from the replacement and
// once from the source line still standing after it.  `pack(1)` twice is
// idempotent, so only the closing token check saw it; the same duplication of
// `push_macro` or `GCC diagnostic push` would have changed program meaning
// silently, and the tool's own `--verify-output` default is `off`.
//
// The trailing trivia after a cover's last token is not part of what the cover
// realizes, which is why the fix does not depend on recognizing the directive:
// see `macro_cover_spanning_pragma_operator_emits_directive_once` for the
// companion case where the directive lies *inside* the cover and is therefore
// the cover's to materialize.
#define GAP2_PRE 1,
#define GAP2_POST 2
int arr[] = { GAP2_PRE
#pragma pack(1)
GAP2_POST };
