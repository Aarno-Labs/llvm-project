// RUN: %clang-refold-tester pragma_operator_midline_folds_back
// A mid-line _Pragma whose text is edited folds back into the inline _Pragma
// operator (using the producer-recorded precise operator byte range), rather
// than being split onto its own #pragma line.
int a; _Pragma("message(\"hi\")") int b;
