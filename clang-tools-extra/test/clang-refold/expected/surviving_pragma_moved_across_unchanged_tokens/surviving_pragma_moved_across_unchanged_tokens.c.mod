// RUN: %clang-refold-tester-verify-off surviving_pragma_moved_across_unchanged_tokens
// A surviving pragma B prints before tokens that A prints before it moves
// across them: the unchanged tokens are deleted on one side of the directive
// and inserted on the other.
//
// No token changes, so no hunk borders the directive.  The placement check
// states the tokens between A's and B's positions as an identity hunk and
// moves them into a pure insertion after the directive.  This used to refuse,
// and before that emitted A unchanged at verify-off.
int x = 1 
#pragma pack(1)
+ 2;
