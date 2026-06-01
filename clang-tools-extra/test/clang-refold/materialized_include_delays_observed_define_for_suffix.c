// RUN: %clang-refold-tester-with-lines materialized_include_delays_observed_define_for_suffix
// Step 2 regression: materializing the include consumes the header-owned
// definition of USE, but the edited header replay intentionally spells USE(3)
// before that definition should be restored for the surviving TU suffix.
#include "headers/defs_and_payload.h"

int suffix = USE(1);
