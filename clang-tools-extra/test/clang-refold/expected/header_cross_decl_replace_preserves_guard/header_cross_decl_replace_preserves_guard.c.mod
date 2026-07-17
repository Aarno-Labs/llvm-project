// RUN: %clang-refold-tester header_cross_decl_replace_preserves_guard
#ifndef HEADER_CROSS_DECL_REPLACE_PRESERVES_GUARD_H
#define HEADER_CROSS_DECL_REPLACE_PRESERVES_GUARD_H

typedef int kept_before_t;
typedef unsigned replacement_t;
typedef int kept_after_t;

#endif

int add_kept_values(kept_before_t before, kept_after_t after) {
  return before + after;
}
