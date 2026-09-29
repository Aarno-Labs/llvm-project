// RUN: %clang-refold-tester-expect-refold-fail verify_output_fatal_fails_without_closing_check
// RUN: FileCheck --input-file=%t/outputs/verify_output_fatal_fails_without_closing_check.out %s
//
// Regression: `--verify-output=fatal` fails when the closing check cannot be
// built, just as it fails when the check cannot be run.
//
// The edited stream adds `#include "verify_output_missing_header.h"`, which no
// producer search path finds -- a transform's generated header whose directory
// was not declared with `--verify-include-dir`.  The edited stream therefore
// cannot be preprocessed and there is no check to run.  The run used to return
// the refold anyway, exit 0, under the one option that asks for it to be
// verified.
//
// CHECK: clang-refold: the closing check could not be built: the edited stream could not be preprocessed; --verify-output=fatal has nothing to compare the refolded source against
int counter = 1;
int main(void) { return counter; }
