// RUN: %clang-refold-tester-verify-off header_pragma_replacement_after_header_lands_before_its_directive
// A replacement just after a header that B prints before the header's own
// trailing pragma is realized inside the header, before that directive.
//
// `+ 2` follows the `#include`; B puts `- 3` between `trailing_a = 1` and the
// header's `#pragma pack(1)`.  The replacement becomes a deletion after the
// include and an insertion at the header's last gap, which B's order forces
// into the header, ahead of the directive.
int trailing_a = 1
- 3
#pragma pack(1)
;
