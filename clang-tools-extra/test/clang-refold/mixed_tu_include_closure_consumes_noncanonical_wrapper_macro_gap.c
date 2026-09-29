// RUN: %clang-refold-tester mixed_tu_include_closure_consumes_noncanonical_wrapper_macro_gap
//
// Regression: an object-like zero-token wrapper whose definition carries a
// comment before its replacement list is consumed with the include closure.
// The neutrality proof used to read the list out of the directive's `text`,
// which drops the comment, so the nested EMPTY lay outside the list it
// computed and the edit refused.  The proof now walks the producer's source
// range for each replacement-list token.
#define KEEP(x) ((x) + 1)
#define EMPTY
#define WRAP_EMPTY /* expands to nothing */ EMPTY

int untouched = KEEP(5);

int arr[] = { 1,
WRAP_EMPTY
#include "two.inc"
};
