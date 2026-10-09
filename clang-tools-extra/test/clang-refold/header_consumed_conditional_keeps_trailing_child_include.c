// RUN: %clang-refold-tester-verify-off header_consumed_conditional_keeps_trailing_child_include
// A header conditional group is not consumed by a deletion that leaves the
// tokens of a child include in its selected arm alive.
//
// B deletes `z` and `a`, which straddle the `#if 1`.  Every token spelled in
// the arm is deleted, but the arm also ran `selected_arm_guarded_child.h`,
// whose `child_t` survives.  Consuming the whole group as a source piece
// deleted the include with it, and this was emitted at verify-off.  The header
// is realized from B instead.
#include "consumed_group_trailing_child_parent.h"
int tail = 3;
