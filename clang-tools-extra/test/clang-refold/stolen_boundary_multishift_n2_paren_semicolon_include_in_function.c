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

#include "headers/ms_parent2.h"

int main(void) {
  ms_outer();
  return 0;
}
