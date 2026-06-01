// RUN: %clang-refold-tester-with-lines balanced_diagnostic_pragma_state_gap
// Step 3 regression: a balanced diagnostic pragma island lies physically
// between two source pieces consumed by one mixed TU/include replacement.
// The island has identity net state at both boundaries, so it can be carried
// as a source-state gap instead of forcing raw-B terminal fallback.
#define LEFT_VALUE 10,

int values[] = {
  LEFT_VALUE
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic pop
#include "headers/right_value2.h"
};
