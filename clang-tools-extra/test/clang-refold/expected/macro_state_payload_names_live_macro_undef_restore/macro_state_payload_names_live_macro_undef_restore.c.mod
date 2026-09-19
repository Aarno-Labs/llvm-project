// RUN: %clang-refold-tester-verify-off macro_state_payload_names_live_macro_undef_restore
// A B payload naming a macro live where it lands is bracketed by a synthetic
// `#undef` and a restore of the definition, when neither can move.
//
// B is a preprocessed stream, so the `ZZ` it carries is an identifier and the
// refold has to keep it one.  `ZZ` is bound at the replay position and expands
// to `VALUE` and thence to `3`, so emitting the payload under that binding
// would re-expand a token B already finished expanding.
//
// Carrying the definition past the replacement is blocked because `int before`
// observes `ZZ` ahead of the edit, and a bare `#undef` is blocked because
// `int later` observes it after.  The repair undefines `ZZ` before the
// payload's line and re-emits the producer's exact `#define ZZ VALUE` right
// after it, so the macro state after the repaired interval equals the state
// before it.  This used to refuse the translation unit, and before that it
// emitted `{ 3 }` at verify-off.
#define VALUE 3
#define ZZ VALUE
int before = ZZ;
#undef ZZ
int arr[] = { ZZ };
#define ZZ VALUE
int later = ZZ;
