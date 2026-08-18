// RUN: %clang-refold-tester-relaxed-expect-refold-fail macro_state_gap_payload_naming_identifier_refuses
// Fail-closed regression: an undetermined payload that names an identifier is
// not insensitive to a `#define`.
//
// Same shape as the straddling-`#define` fold, but the payload is `ZZ` rather
// than a number.  A macro definition is reachable only through an identifier,
// and an identifier can reach a definition it does not spell -- another macro
// live at the replay position may expand to the name -- so comparing the
// payload against the directive's own spelling would prove nothing.  The rule
// is therefore the conservative one: any identifier in the committed payload
// makes the placement observable, and the side must be determined by something
// other than this theorem.
//
// PINS A REFUSAL, NOT AN OUTPUT.  It exists so that widening the macro-state
// exemption cannot quietly start choosing a side for payload whose two
// placements are not proven equivalent.  Deciding this one needs macro-liveness
// facts, not a wider default.
int arr[] = { 1,
#define VALUE 3
2 };
