// RUN: %clang-refold-tester pragma_once_single_occurrence_deletes_pragma
// Regression: with exactly one occurrence of the header nothing can re-enter or
// duplicate it, so no guard is emitted.  The inlined `#pragma once` is deleted
// instead, because it is inert in the main file and would otherwise emit
// -Wpragma-once-outside-header.  Replacing only the directive spelling keeps the
// edit line-neutral.

#define GUARD_SOLO_V 5
int from_guard_solo = 8;

int tail = GUARD_SOLO_V;
