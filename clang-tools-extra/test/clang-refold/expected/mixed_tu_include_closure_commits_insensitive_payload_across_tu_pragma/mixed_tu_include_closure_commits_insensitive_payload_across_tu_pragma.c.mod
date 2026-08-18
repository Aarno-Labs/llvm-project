// RUN: %clang-refold-tester mixed_tu_include_closure_commits_insensitive_payload_across_tu_pragma
// Payload-insensitivity regression: one B token `3` replaces A's `1 , 2`, whose
// A range straddles the preserved `#pragma GCC poison FOO`.  The alignment
// cannot place `3` -- it is edited material with no A-side position -- so the
// partition alone refuses.  `3` is a numeric constant and names no poisoned
// identifier, so both placements re-preprocess to the same translation unit and
// committing one deterministically is a proof rather than a guess.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 3
#pragma GCC poison FOO
};
