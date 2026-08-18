// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_system_header_pragma_gap
// Consumability regression: the gap include's only recorded structure is
// `#pragma GCC system_header`, which suppresses diagnostics from that point in
// the file that carries it and does not leak to the includer.  Consuming the
// zero-token include deletes the pragma and every byte in its scope together,
// so nothing survives to observe it and the closure need not surrender.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#include "system_header_gap.inc"
#include "two.inc"
};
