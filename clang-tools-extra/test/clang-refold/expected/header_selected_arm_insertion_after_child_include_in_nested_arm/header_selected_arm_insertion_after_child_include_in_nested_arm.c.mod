// RUN: %clang-refold-tester-verify-off header_selected_arm_insertion_after_child_include_in_nested_arm
// The tokens a header arm produced include those of a child include sitting
// in an arm nested inside it.
//
// The child include is wrapped in `#if 1`, so neither the outer arm's spelled
// span nor the nested group's own record mentions its tokens.  An insertion
// after them, before `parent_decl`, still lies inside the outer arm and must
// not be placed before its `#ifndef`.
#ifndef SELECTED_ARM_NESTED_CHILD_PARENT_H
#define SELECTED_ARM_NESTED_CHILD_PARENT_H
#if 1
#include "selected_arm_guarded_child.h"
typedef child_t *child_ref;

#endif
int parent_decl(void);
#endif
int tail = 3;
