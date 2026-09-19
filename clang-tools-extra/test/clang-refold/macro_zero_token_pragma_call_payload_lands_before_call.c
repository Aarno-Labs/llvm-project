// RUN: %clang-refold-tester-verify-off macro_zero_token_pragma_call_payload_lands_before_call
// A replacement after a call whose whole expansion is a printed `_Pragma`
// lands before the call.
//
// `P` expands to no tokens and performs only a printed pragma, so it changes
// no preprocessor state and the payload may be placed on either side of it;
// B puts `- 3` first.
#define P _Pragma("pack(1)")
int x = 1
P
+ 2;
