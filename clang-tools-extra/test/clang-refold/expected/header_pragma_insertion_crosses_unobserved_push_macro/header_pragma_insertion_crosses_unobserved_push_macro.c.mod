// RUN: %clang-refold-tester-verify-off header_pragma_insertion_crosses_unobserved_push_macro
// An insertion B prints after a header's pragma lands after it across a
// consumed `push_macro` the payload cannot observe.
//
// The gap before `pushed_h` holds a `#define` and a `push_macro` of that
// name, which print nothing, and the printed `#pragma pack(1)`.  The payload
// names neither macro, so every site in the gap preprocesses it alike and the
// one beside the pragma is sound.  This used to refuse the translation unit.
#define PUSHED_HEADER_X 1
#pragma push_macro("PUSHED_HEADER_X")
#pragma pack(1)
int pushed_ins;
int pushed_h = PUSHED_HEADER_X;
int pushed_z;
