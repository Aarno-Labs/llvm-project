// RUN: %clang-refold-tester pragma_operator_message_content_folds_back
// Editing the message string of a _Pragma folds back into the _Pragma
// operator (re-stringized), not a raw #pragma.
_Pragma("message(\"bye\")")
int a = 1;
