// RUN: %clang-refold-tester-verify-off macro_pragma_at_call_edge_whole_cover_replays_line
// Expanding a call whose `_Pragma` B prints at the edge of the call's tokens
// replays that pragma line.
//
// Whole cover replaces the call with B's tokens, and exact token coverage
// stops at the first and last token, which left the line behind: `S2` and `S3`
// both used to lose their pragma at verify-off.  The call is the only source
// of the line, so the realization now carries B's copy at the leading edge
// for `S2` and the trailing edge for `S3`.
#define S2 _Pragma("pack(1)") 1
#define S3 1 _Pragma("pack(1)")
int x = S2;
int y = S3 + 2;
