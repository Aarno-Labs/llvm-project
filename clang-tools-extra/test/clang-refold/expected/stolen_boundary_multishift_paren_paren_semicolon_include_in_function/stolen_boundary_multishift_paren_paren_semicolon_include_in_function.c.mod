// RUN: %clang-refold-tester-with-lines stolen_boundary_multishift_paren_paren_semicolon_include_in_function

// test_ms3.c: multi-token stolen-boundary repro (expects multi-pass repair)
//
// Layout:
//   ms_parent.h emits a statement whose tail tokens are ") ) ;" via macro
//   expansion, then immediately includes ms_child.h *inside the same function
//   body*.  The child file starts with an empty compound statement "{ }".
//
// Intended edit in the *preprocessed* output:
//   Insert another macro-expanded statement, e.g. "((void)(0));", immediately
//   before the "{ }" block from ms_child.h.
//
// Expected behavior:
//   A single-step stolen-boundary repair will only shift one token (typically
//   the first ')'), leaving the insertion still starting with a stolen ')' or
//   ';'.  A correct multi-step repair should shift across the entire tail
//   sequence (two ')' plus ';').

#line 1 "./headers/ms_parent.h"
#pragma once

// Keep semicolons scarce and concentrate the boundary right before the include.
// We intentionally create a tail ")) ;" sequence via macro expansion.

#define ms_unique_arg_123 123
#define ms_call_unique(x) ((void)(x))

static inline void ms_trigger(void) {
  // This statement expands to: ((void)(123));
  // The seam immediately after it is where the insertion is intended.
  ms_call_unique(ms_unique_arg_123);

  // Include *inside* the function body so the next tokens in the PP stream come
  // directly from the child snippet.
)); ((void)(0)); /* pure insertion at before boundary for ms_child_begin_marker */
#line 16 "./headers/ms_parent.h"
#include "ms_child.h"
}
#line 21 "stolen_boundary_multishift_paren_paren_semicolon_include_in_function.c"

int main(void) {
  // Semicolon-free TU body to reduce alternative ';' matches.
  // (Empty compound statement is valid C.)
  {
  }
}
