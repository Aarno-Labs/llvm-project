// RUN: %clang-refold-tester-verify-off macro_body_pragma_operator_arg_edit_keeps_directive_out_of_args
// Editing the argument of a call whose body prints a `_Pragma` must not
// splice B's printed `#pragma` line into the argument list.
//
// `F(2)` expands to `2`, then the pragma line, then `+ 1`.  B edits the
// argument to `3`, and B's copy of the pragma line sits between `3` and `+`,
// inside the B range the call realizes.  An args-only rewrite takes the
// argument's B envelope, which runs up to the next token and so carries that
// line: it used to emit
//
//   int x = F(3
//   #pragma pack(1));
//
// which does not preprocess.  The call is the only source of the line, so a
// candidate keeping the callsite has to show where re-expansion prints it, and
// none can:
//
//   reject inv id=... name=F origin=DirectArgsOnly
//   reason=printed-pragma-not-replayed
//
// The whole-cover realization replays B's bytes across the call, line
// included, and is what remains.
#define F(a) a _Pragma("pack(1)") + 1
int x = 3
#pragma pack(1)
 + 1;
