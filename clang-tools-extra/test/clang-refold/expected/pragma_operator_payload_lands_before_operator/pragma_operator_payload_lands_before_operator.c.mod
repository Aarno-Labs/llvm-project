// RUN: %clang-refold-tester-verify-off pragma_operator_payload_lands_before_operator
// A replacement after a `_Pragma` operator that B prints before the pragma
// lands before the operator.
//
// The operator is spelled in translation-unit text and prints nothing but its
// pragma, so the replacement is split into an insertion before it and a
// deletion after it, as for a `#pragma` directive.  This used to emit the
// operator before `- 3` at verify-off.
int x = 1
 - 3
_Pragma("pack(1)")
;
