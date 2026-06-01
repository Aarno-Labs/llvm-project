// RUN: %clang-refold-tester-with-lines conditional_source_slot_empty_selected_arm
// Step 4 regression: the included header has a selected conditional arm
// that produced no A-side PP tokens.  A B-only insertion at the collapsed
// PP gap must use the producer's arm_begin source slot rather than falling
// back to raw B or inserting before the include in the TU.
#define ENABLE 1
#include "empty_selected_arm.h"
