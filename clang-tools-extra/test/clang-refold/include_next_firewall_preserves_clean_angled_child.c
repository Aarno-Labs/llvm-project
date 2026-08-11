// RUN: %clang-refold-tester-clang-flags include_next_firewall_preserves_clean_angled_child -- -I %S/headers/include_next_firewall/a -I %S/headers/include_next_firewall/b
// Regression: forced include-next materialization must follow relocation, not
// reachability.  `mid.h` is quoted, so a materialized parent cannot preserve it
// and it is materialized -- and because its subtree spells `#include_next`, the
// subtree-wide force flag used to make every descendant materialize too.  That
// caught `<clean.h>`, an angled child with no descendant work of its own, whose
// directive line never moves.
//
// A preserved `#include` is a firewall: the compiler opens the real file at its
// real location, so the `#include_next` inside `clean.h` resumes from the same
// HeaderSearch cursor the original used.  `<clean.h>` must therefore survive as
// a directive, and its body must not appear inline.  The `#include_next` edge
// itself still relocates with its parent's text and is still materialized.
#include "headers/include_next_firewall/outer/parent.h"
int main(void) { return p + q + m + c + d; }
