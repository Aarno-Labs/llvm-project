// RUN: %clang-refold-tester-clang-flags-verify-off relocated_skipped_include_once_skip_realizes_header_from_b -- -I headers/relocated_skipped_include_once_skip -I headers/relocated_skipped_include_once_skip/decoy
// `orig/a.h` must be inlined, which moves its `#include "sub/leaf.h"` beside
// the output, where the operand finds `decoy/sub/leaf.h`. Clang skipped that
// include, but not because of `RSIO_LEAF_H`, which was undefined by then:
// `leaf.h` holds `#pragma once` inside its guard, and Clang tests include-once
// state first. Wrapping the directive in `#ifndef RSIO_LEAF_H`, as
// relocated_skipped_include_guarded_by_controlling_macro does, would admit it
// and include the decoy. The directive did nothing in the producer, so
// `orig/a.h` is realized from B, which drops it.
#include "orig/sub/leaf.h"
#undef RSIO_LEAF_H
int rsio_a = 3;
int main(void) { return rsio_a + rsio_leaf; }
