// RUN: %clang-refold-tester macro_state_gap_payload_naming_unbound_identifier_commits
// An undetermined payload naming an identifier no `#define` binds is
// insensitive to a `#define` it straddles.
//
// Same shape as the straddling-`#define` fold, but the payload is `ZZ` rather
// than a number.  A macro definition is reachable only through an identifier,
// so a payload has to be read for identifiers -- and an identifier can reach a
// definition it does not spell, because an identifier that is itself a live
// macro expands to a replacement list that may name one.  Comparing the payload
// against the directive's own spelling alone would therefore prove nothing.
//
// The producer records every `#define` the preprocessor saw, which closes that
// gap without weakening it: an identifier no record binds cannot be live, so it
// stands for itself and reaches nothing.  `ZZ` is such an identifier, and it is
// not `VALUE`, so both placements re-preprocess to the same tokens and the
// payload is committed after the directive.
//
// The companion refusal, `macro_state_gap_payload_naming_macro_identifier_refuses`,
// pins the other side: an identifier a `#define` does bind keeps failing closed.
int arr[] = { 1,
#define VALUE 3
2 };
