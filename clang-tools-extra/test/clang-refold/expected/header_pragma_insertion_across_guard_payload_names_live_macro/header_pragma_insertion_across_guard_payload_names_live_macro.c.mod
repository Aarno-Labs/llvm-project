// RUN: %clang-refold-tester-verify-off header_pragma_insertion_across_guard_payload_names_live_macro
// Crossing an include guard to reach a printed pragma does not license writing
// a payload under a live macro it spells.
//
// The guard's directives are proven crossable, because the payload names
// neither guard macro, but `GUARD_NAMED_W` was defined before the gap and is
// still live at the site.  The gap-crossing proof asks only about the gap's
// own structures, so the header payload obligation must refuse the site.
int guard_named_a;
int guard_named_w = 5;
#pragma pack(1)
int GUARD_NAMED_W;
int guard_named_h;
