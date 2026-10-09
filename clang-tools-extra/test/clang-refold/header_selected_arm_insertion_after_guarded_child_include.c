// RUN: %clang-refold-tester-verify-off header_selected_arm_insertion_after_guarded_child_include
// An insertion right after a child include that opens a header's selected arm
// is realized after that include, not before the arm's conditional group.
//
// The arm's recorded span counts only the tokens spelled in the header, so it
// begins at `parent_decl` although the child include produced tokens first.
// Taking that span as the arm's extent placed `child_ref` before `#ifndef`,
// ahead of the `child_t` it names, and this was emitted at verify-off.
#include "selected_arm_child_first_parent.h"
int tail = 3;
