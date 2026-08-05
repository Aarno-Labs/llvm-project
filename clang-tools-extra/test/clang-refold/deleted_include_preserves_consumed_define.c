// RUN: %clang-refold-tester deleted_include_preserves_consumed_define
// An edit that deletes everything an include contributed must not simply drop
// the `#include`: the header also defined a macro that a surviving callsite
// still consumes.  A directive is preprocessor state, not a replaceable byte,
// so the definition has to survive the deletion of the tokens around it.
#include "state_define_carrier.h"
int state_define_use = STATE_CARRIED;
