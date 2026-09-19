// RUN: %clang-refold-tester-verify-off header_pragma_payload_lands_before_directive_inside_header
// A replacement inside a header that B prints before one of the header's own
// surviving pragmas lands before that directive, inside the header.
//
// The header's `#pragma pack(1)` is bound in the header's own census and
// changes no state, so the replacement is split into an insertion before it and
// a deletion after it, and the header planner places the insertion at the
// directive.  This used to emit the directive before `- 3` at verify-off.
int inside_a = 1
- 3
#pragma pack(1)
;
int inside_z;
