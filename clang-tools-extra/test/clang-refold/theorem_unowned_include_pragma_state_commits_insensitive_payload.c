// RUN: %clang-refold-tester theorem_unowned_include_pragma_state_commits_insensitive_payload
// Companion to the mixed-closure insensitivity case, entered through the
// unowned-include pragma-state theorem: the same straddling payload is placed
// by insensitivity rather than by alignment.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#include "two.inc"
};
