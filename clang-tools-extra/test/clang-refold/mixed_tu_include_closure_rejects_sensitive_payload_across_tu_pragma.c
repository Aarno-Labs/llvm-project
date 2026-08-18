// RUN: %clang-refold-tester mixed_tu_include_closure_rejects_sensitive_payload_across_tu_pragma
// Fail-closed companion to the payload-insensitivity theorem: the same
// straddling shape, but the edited payload names the poisoned identifier.  Only
// one placement re-preprocesses at all, so the two are not equivalent and the
// alignment still cannot say which is meant.  The closure must refuse rather
// than commit the placement that happens to compile.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#include "two.inc"
};
