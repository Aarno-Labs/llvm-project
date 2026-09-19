// RUN: %clang-refold-tester-verify-off header_payload_naming_header_macro_is_not_inserted_under_it
// A header insertion whose B payload spells a macro the header defines is not
// written into the header after that definition.
//
// B is preprocessed, so the inserted `NAMED_V` is an identifier.  Written
// after `#define NAMED_V 3` it would expand to `3`.  No stage audits a header
// edit for macro liveness, so the header planner refuses the payload and the
// include is realized another way.  This used to emit `int 3;` at verify-off.
int named_a;
int named_h;
int NAMED_V;
int named_g = 3;
