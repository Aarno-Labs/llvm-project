#include "edit_map_once_mid.h"
// A second occurrence of the leaf, suppressed by its `#pragma once`. Without
// it the leaf has a single recorded occurrence and takes the delete-pragma
// treatment, which is a different theorem from the one under test.
#include "edit_map_once_leaf.h"
int eom_parent_value = 33;
