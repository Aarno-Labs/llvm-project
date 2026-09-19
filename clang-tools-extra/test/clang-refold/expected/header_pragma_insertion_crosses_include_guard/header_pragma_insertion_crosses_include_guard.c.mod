// RUN: %clang-refold-tester-verify-off header_pragma_insertion_crosses_include_guard
// An insertion B prints after a header's pragma lands after it even though the
// header's include guard shares the gap.
//
// The gap before `guarded_h` holds the guard's `#ifndef` and `#define` as
// well as the printed `#pragma pack(1)`.  Neither guard directive prints
// anything, so B orders the payload only against the pragma; the site beside
// the pragma is proven equivalent to any other in the gap because the payload
// cannot observe the guard macro and lands in the arm the guard selected.
// This used to refuse the translation unit.
int guarded_a;
#ifndef PRAGMA_GUARDED_HEADER_INSERT_H
#define PRAGMA_GUARDED_HEADER_INSERT_H
#pragma pack(1)
int guarded_ins;
int guarded_h;
#endif
