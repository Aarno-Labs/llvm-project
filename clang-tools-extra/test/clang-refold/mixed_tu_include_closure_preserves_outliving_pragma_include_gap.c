// RUN: %clang-refold-tester mixed_tu_include_closure_preserves_outliving_pragma_include_gap
// Directive-preservation regression: the gap include's header carries
// `#pragma GCC poison`, whose state applies to the rest of the translation unit
// and so cannot be consumed with the include.  Preserving the directive in
// place re-enters the header at its original position, which re-runs that
// transition exactly where it ran before -- sound where deleting it is not, and
// strictly better than refusing, which would surrender every directive in the
// file to avoid dropping this one.  The payload `3` names no poisoned
// identifier, so its side of the directive is settled by insensitivity.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#include "poison_gap.inc"
#include "two.inc"
};
