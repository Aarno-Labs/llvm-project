// RUN: %clang-refold-tester-verify-off header_payload_naming_header_macro_ambiguous_realization_keeps_header_tokens
// A B payload naming a macro the header defined earlier is not written under
// that definition, and realizing the include instead keeps the header's tokens.
//
// The alignment cannot tell whether B's new `int V ;` follows `h` or precedes
// it, and one reading makes it a header replacement that would emit `V` under
// its definition.  Refusing that reading realizes the include from B, whose
// slice is checked against the certified token map: the byte diff, which picks
// its own alignment among the single-letter spellings here, once dropped
// `int h;`.
int a;
int h;
int V;
int g;
#define V 3
