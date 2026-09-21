// RUN: %clang-refold-tester-with-lines stress_materialized_include_multi_carried_definition_line_prune

// One carried definition is not the whole rule.  The prefix the macro-state
// repair prepends is the concatenation of every definition it carries, so the
// displacement it imposes on the body's line-control candidates is the sum of
// three directive lines here, not one, and the function-like member makes the
// prefix's own shape non-uniform.  Fold the initializer so all three are
// consumed and must travel out in front of the materialized body, then check
// that the entry directive is still pruned at its own bytes and the header
// guard survives intact.
#include "carried_definition_line_prune_stress_outer.h"
int result = LINE_PRUNE_EOF;
int sized = LINE_PRUNE_BUF;
int tagged = LINE_PRUNE_TAG(4);
