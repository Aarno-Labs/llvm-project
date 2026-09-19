// RUN: %clang-refold-tester-verify-off header_pragma_payload_naming_header_macro_is_not_inserted_under_it
// An insertion placed beside a header's printed pragma is not written under a
// live definition of a macro its B payload spells.
//
// The pragma fixes the payload's site after `#pragma pack(1)`, where
// `#define PRAGMA_NAMED_W 5` is live, so the B identifier `PRAGMA_NAMED_W`
// would expand.  This used to emit `int 5;` at verify-off.
int pragma_named_a;
int pragma_named_w = 5;
#pragma pack(1)
int PRAGMA_NAMED_W;
int pragma_named_h;
