// RUN: %clang-refold-tester-verify-off materialized_header_macro_state_brackets_payload_when_prefix_reads_indirectly
// Regression: an include-owned macro patch whose B payload spells `VAL` must
// see `VAL` undefined, and preserved header source on the payload's line must
// keep seeing it defined.  The wider repair undefines `VAL` before the whole
// line and restores it after, which is sound only when the rest of the line
// does not need `VAL`.  `IND +` reads it through `IND` without spelling it, so
// that repair made `IND` the enumerator 3 where B has 99.  Its check scanned
// the line's text alone; it now also asks whether the producer recorded an
// expansion of `VAL` there, refuses, and the narrower repair brackets only the
// payload instead.  The enumerator keeps the edited stream valid C.
enum { VAL = 3 };
// Header for materialized_header_macro_state_brackets_payload_when_prefix_reads_indirectly.
//
// The edit lands inside `ID(4242)`, and `IND +` before it on the same line
// reads `VAL` through `IND` without spelling it.
#define VAL 99
#define IND VAL
#define ID(x) (x)
int patched(void) { return IND + 
#undef VAL
(VAL)
#define VAL 99
; }
int tu(void) { return 0; }
