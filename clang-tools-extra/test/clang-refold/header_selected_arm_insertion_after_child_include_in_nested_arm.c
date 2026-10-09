// RUN: %clang-refold-tester-verify-off header_selected_arm_insertion_after_child_include_in_nested_arm
// The tokens a header arm produced include those of a child include sitting
// in an arm nested inside it.
//
// The child include is wrapped in `#if 1`, so neither the outer arm's spelled
// span nor the nested group's own record mentions its tokens.  An insertion
// after them, before `parent_decl`, still lies inside the outer arm and must
// not be placed before its `#ifndef`.
#include "selected_arm_nested_child_parent.h"
int tail = 3;
