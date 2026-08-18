// RUN: %clang-refold-tester mixed_tu_include_closure_preserves_pragma_once_with_macro_state_gap
// Companion case: the gap include's header owns `#pragma once` beside a
// `#define`, so its state is neither consumable nor droppable.  Preserving the
// directive keeps both transitions at their original position, which is why
// this no longer needs the pragma-once exception to admit the surrounding
// macro state.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#include "pragma_once_define_gap.inc"
#include "two.inc"
};
