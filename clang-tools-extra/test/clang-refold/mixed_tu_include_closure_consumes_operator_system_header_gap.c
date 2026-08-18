// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_operator_system_header_gap
// Producer regression: the operator spelling of a *consumed* pragma must reach
// the refold map, or a header carrying nothing else looks like a header
// carrying nothing at all.
//
// Clang handles `GCC system_header` inside its own handler, so an operator
// spelling of it reaches neither the generic `#pragma` hook nor the printing
// path that reconstructs a spelling for passed-through pragmas.  It was
// recorded nowhere.  This gap include's only content is that pragma, so the
// closure saw a zero-token include with no recorded side effects at all and
// consumed it as an inert gap -- reaching the right answer with no evidence for
// it, and reaching the same answer for any other consumed state spelled the
// same way.
//
// The producer now records the operator event itself, so the include carries a
// recorded side effect and consumption is decided by the consumability proof:
// `GCC system_header` is diagnostic state scoped to the file that carries it,
// so consuming the include deletes the pragma and every byte in its scope
// together and nothing survives to observe it.  Same outcome as the directive
// spelling in `mixed_tu_include_closure_consumes_system_header_pragma_gap`,
// now for the same stated reason.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#include "system_header_gap_operator.inc"
#include "two.inc"
};
