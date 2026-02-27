// RUN: %clang-refold-tester-with-lines stolen_boundary_multishift_n2_paren_semicolon_include_in_function

// multishift_minimal_v3: forces 2-step stolen-boundary seam repair (')' then ';')
//
// After preprocessing, insert the following line *immediately before*
//   int ms_child_begin_marker = 42;
// in the preprocessed output:
//   other_call_unique(0); /* pure insertion at before boundary for ms_child_begin_marker */
//
// The optimal owner-aware alignment may match the parent statement's closing ") ;"
// to the inserted statement's closing ") ;", stealing TWO boundary tokens.
// Single-pass repair can only shift once (')'), leaving the insertion before ';'.
// Multi-pass repair can shift twice (')' then ';') and land after the full statement.

#line 1 "./headers/ms_parent2.h"
#pragma once

// Keep these names unique-ish to discourage earlier token alignment drift.
static inline void ms_call_unique(int x) { (void)x; }
static inline void other_call_unique(int x) { (void)x; }

static inline void ms_outer(void) {
  int ms_unique_arg_123 = 7;

  // This statement is in the PARENT include (depth=1).
  // It ends with the safe-boundary suffix: ")" ";".
  ms_call_unique(ms_unique_arg_123);

  // Include seam INSIDE a function body.
  // The marker declaration below lives in the CHILD include (depth=2).
other_call_unique((0)); /* pure insertion at before boundary for ms_child_begin_marker */
#line 16 "./headers/ms_parent2.h"
#include "ms_child2.h"

  other_call_unique(ms_unique_arg_123);
}
#line 16 "stolen_boundary_multishift_n2_paren_semicolon_include_in_function.c"

int main(void) {
  ms_outer();
  return 0;
}
