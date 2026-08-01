// RUN: %clang-refold-tester realized_from_b_restores_dead_include_guard
// Regression: `breal_has_include.h` uses __has_include, so its body is realized
// from the edited preprocessed stream rather than replayed from source.  A
// realized body is tokens, so every directive it contained is gone, including
// the `#define BREAL_GUARDED_H` that `breal_guarded.h`'s include guard sets.
// The later `#include "breal_later.h"` would therefore re-enter that header and
// redefine the typedef.
//
// The producer-recorded controlling_macro is re-established beside the realized
// body, which reproduces the original skip exactly.  Restoration is admitted
// only because `breal_guarded.h`'s subtree defines no macro observed outside it:
// restoring a guard suppresses re-entry into the header *and everything it
// includes*, so a header whose macro state is live elsewhere must instead fail
// closed.
typedef struct BRealGuarded { int v; } BRealGuarded;

int breal_has = 1;
int breal_val = 9;

#ifndef BREAL_GUARDED_H
#define BREAL_GUARDED_H
#endif

int mid = 0;

#include "breal_later.h"
