// RUN: %clang-refold-tester-verify-off header_pragma_insertion_at_header_start_lands_after_its_directive
// An insertion at a header's first gap that B prints after the header's own
// leading pragma is realized inside the header.
//
// An insertion at an include's boundary normally belongs to the parent and so
// lands before the whole header.  Here B prints the header's own line first,
// so the payload sits between that line and the rest of the header, which only
// the header can realize.  This used to emit `leading_ins` before the directive
// at verify-off.
int leading_a;
#pragma pack(1)
int leading_ins;
int leading_h;
