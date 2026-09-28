// RUN: %clang-refold-tester tu_replacement_consumes_include_only_taken_arm
//
// Regression: a replacement whose A tokens include those of a header brought
// in by an `#if` arm holding nothing but that `#include`.  The arm printed no
// token of its own, so the token-overlap `selected` flag called it not taken,
// the header's tokens belonged to no arm, and the refold refused.  The
// preprocessor's own `taken` puts them in the arm, so the replacement consumes
// the whole conditional group.
int z = 9;
