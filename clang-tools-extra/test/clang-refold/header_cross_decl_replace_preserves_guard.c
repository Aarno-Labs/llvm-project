// RUN: %clang-refold-tester header_cross_decl_replace_preserves_guard
#include "header_cross_decl_replace_preserves_guard.h"

int add_kept_values(kept_before_t before, kept_after_t after) {
  return before + after;
}
