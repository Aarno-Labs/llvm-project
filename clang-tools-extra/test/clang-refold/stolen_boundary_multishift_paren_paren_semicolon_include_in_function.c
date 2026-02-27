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

#include "headers/ms_parent.h"

int main(void) {
  // Semicolon-free TU body to reduce alternative ';' matches.
  // (Empty compound statement is valid C.)
  {
  }
}
