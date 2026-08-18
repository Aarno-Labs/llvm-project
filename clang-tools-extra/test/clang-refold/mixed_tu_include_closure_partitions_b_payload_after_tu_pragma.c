// RUN: %clang-refold-tester mixed_tu_include_closure_partitions_b_payload_after_tu_pragma
// B-payload/preserved-source partition regression: a mixed TU/include closure
// spans a `#pragma GCC poison` whose net state is not identity, so it can
// neither be consumed nor relocated.  Every B token in the closure's material
// aligns to an A token after the pragma, so the payload is cut at the pragma's
// A frontier and the directive is emitted in place ahead of it instead of
// surrendering the translation unit to raw B.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#include "two_seven.inc"
};
