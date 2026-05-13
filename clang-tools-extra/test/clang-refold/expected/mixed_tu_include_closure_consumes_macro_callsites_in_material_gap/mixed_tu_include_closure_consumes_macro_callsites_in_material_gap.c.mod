// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_macro_callsites_in_material_gap
#define KEEP(x) ((x) + 1)
#define EMPTY
#define FORWARD(x) x

int untouched = KEEP(5);

int arr[] = { 3
#if 1
#if 1
FORWARD(EMPTY)
#endif
#endif
};
