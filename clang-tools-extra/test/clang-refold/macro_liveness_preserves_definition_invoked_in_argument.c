// RUN: %clang-refold-tester macro_liveness_preserves_definition_invoked_in_argument

// Deleting the header's last declaration together with the TU declaration
// after the include gives the include up, so every #define the header held is
// consumed by that edit.  IDENTITY(SHIFT_BITS) survives and keeps IDENTITY's
// definition, but the producer records SHIFT_BITS as an invocation nested in
// IDENTITY's expansion rather than as a callsite of its own.  Its definition
// is still needed where the call re-expands.
#include "liveness_nested_argument.h"
static const char *names[2] = {
};
int g(int v) { return v >> IDENTITY(SHIFT_BITS); }
