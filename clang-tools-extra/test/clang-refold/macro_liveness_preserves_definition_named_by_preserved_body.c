// RUN: %clang-refold-tester macro_liveness_preserves_definition_named_by_preserved_body

// Deleting the header's last declaration together with the TU declaration
// after the include gives the include up, so every #define the header held is
// consumed by that edit.  GET_HIGH(v) survives and keeps its definition, but
// SHIFT_BITS is only ever invoked from GET_HIGH's expansion.  Keeping
// GET_HIGH alone leaves SHIFT_BITS undefined, so it survives the expansion as
// a bare identifier where the edited stream has `5`.
#include "liveness_nested_body.h"
static const char *names[2] = {
};
int g(int v) { return GET_HIGH(v); }
