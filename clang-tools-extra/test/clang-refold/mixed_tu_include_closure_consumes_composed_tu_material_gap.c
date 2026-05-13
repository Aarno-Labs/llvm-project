// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_composed_tu_material_gap
#define KEEP(x) ((x) + 1)
#define EMPTY
#define FORWARD(x) x

int untouched = KEEP(5);

int arr[] = { 1,
#if 1
#if 1
FORWARD(EMPTY)
#endif
#endif
#if 1
 KEEP(10),
#endif
#include "two.inc"
};
