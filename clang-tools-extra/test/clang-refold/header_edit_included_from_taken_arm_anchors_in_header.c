// RUN: %clang-refold-tester header_edit_included_from_taken_arm_anchors_in_header
//
// Regression: an edit inside a header included from a taken `#if` arm is
// anchored in the header's own source.  The edit's owner arm used to be the
// includer's arm around the `#include`, since arm lookup by token walks out
// through include sites; no byte of the header lies in that arm, so every
// header anchor was refused and the header was realized from the edited
// stream instead.  An include owner now keeps only an arm of its own instance.
// The first arm holds only the `#include`; the second also has a token.
int a = 1;
#if 1
#include "taken_arm_only_include.h"
#endif
#if 1
int x = 0;
#include "taken_arm_with_tokens_include.h"
#endif
int b = 2;
