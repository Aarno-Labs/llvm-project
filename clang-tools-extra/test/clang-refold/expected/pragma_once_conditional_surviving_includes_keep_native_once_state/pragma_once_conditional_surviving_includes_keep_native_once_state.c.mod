// RUN: %clang-refold-tester-verify-off pragma_once_conditional_surviving_includes_keep_native_once_state
// Regression: a header with a conditional `#pragma once` is inlined at one
// occurrence and kept as a directive at the others.
//
// The edit is in the first occurrence, so that copy is inlined with its once
// site rewritten to a `#define` of the synthetic guard macro inside the arm.
// The arm is not taken there, so the macro stays undefined.  The two later
// `#include`s contribute no edit and survive, each wrapped in an `#ifndef` of
// that macro.
//
// The wrappers do not define the macro eagerly.  An eager define marks the
// header as included whether or not its pragma fires, and here the pragma's
// condition changes between inclusions.  Instead the second directive enters
// the unmodified header, its arm is taken, and Clang's own once-state
// suppresses the third, exactly as in the original.  The macro and Clang's
// once-state each record the pragma executions of one kind of occurrence.
// That split is sound only because no inlined copy follows a surviving
// directive: an inlined copy would consult the macro alone.
//
// Before this rule, a conditional pragma refused every surviving wrapper, and
// the fallback inlined all three occurrences.
#ifndef __CLANG_REFOLD_ONCE_1
// `#pragma once` that fires only once the includer has defined
// GUARD_ONCE_ARMED, so an earlier inclusion re-enters and a later one does not.
#ifdef GUARD_ONCE_ARMED
#define __CLANG_REFOLD_ONCE_1
#endif
int guard_once_armed_body = 7;
#endif
int mid = 0;
#define GUARD_ONCE_ARMED
#ifndef __CLANG_REFOLD_ONCE_1
#include "guard_once_ifdef_armed.h"
#endif
int mid2 = 0;
#ifndef __CLANG_REFOLD_ONCE_1
#include "guard_once_ifdef_armed.h"
#endif
int tail = 2;
