// RUN: %clang-refold-tester-clang-flags selfref_macro_carry_across_observing_payload -- -I %S/headers/selfref_carry
// Regression: a definition that expands to itself may be carried across a
// payload that names it.
//
// `defs.h` uses `__has_include`, so it cannot be materialized from source and
// is realized from the edited stream instead -- which drops its `#define`.  The
// TU's suffix still names the macro, so the definition has to be carried out in
// front of the materialized payload.  The payload also names it, which normally
// forbids the carry: a payload's tokens are already expanded, so making a name
// expandable again would change how it re-preprocesses.
//
// `#define selfref_obj selfref_obj` is the exception.  The name is not
// re-expanded during its own replacement, so expansion is the identity and the
// payload is unaffected.  Without that theorem this whole translation unit
// emits a verbatim copy of the edited stream.
#include "headers/selfref_carry/parent.h"
int suffix_use = selfref_obj;
