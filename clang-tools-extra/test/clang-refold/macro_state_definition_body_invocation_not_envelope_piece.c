// RUN: %clang-refold-tester macro_state_definition_body_invocation_not_envelope_piece

// WRAP is invoked twice, so the producer records two DOUBLE invocations whose
// callsite bytes are both the DOUBLE(a) written in WRAP's replacement list.
// Admitting those as source-envelope pieces yields two pieces over one
// identical byte range, which no piece order can resolve, and the whole
// envelope census collapses.  A callsite inside a #define is macro-definition
// state, not a distinct source occurrence, and contributes no piece.
int untouched = 1;

#include "body_generated_pieces.h"
