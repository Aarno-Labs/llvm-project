// RUN: %clang-refold-tester-with-lines materialized_header_macro_state_undef_restore_preserves_line_observers
//
// Line-observer companion to
// `materialized_header_macro_state_undefines_and_restores_across_observer`.
//
// The undefine/restore repair is the only include-owned macro-state repair that
// *adds* physical lines: the carry repair moves a directive line and preserves
// the body's line count, while this one synthesizes an `#undef` before the
// replacement's line and a `#define` after it.  Two added lines shift every
// `__LINE__` after them in the materialized body and in the translation unit,
// so the repair must compose with line-control repair rather than assume it.
//
// `__LINE__` is read on both sides of the repaired line and after the include,
// which pins the observed values against the emitted `#line` state.
#include "materialized_header_macro_state_line_observer.h"
int tu_line = __LINE__;
