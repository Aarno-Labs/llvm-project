// RUN: %clang-refold-tester realized_from_b_header_line_delete_ignores_nested_includes
// Regression: deleting a line inside a conditional group of
// `rfb_nested_inner.h` realizes it from the edited preprocessed stream, and so
// is its includer `rfb_nested_outer.h`, whose `__has_include` cannot be
// replayed at the translation-unit site.  `rfb_nested_types.h` is guarded, but
// its macro is observed in `rfb_nested_inner.h`, so its guard is not restored
// beside the realized body.
//
// Every include that could re-enter it -- the entered edges in
// `rfb_nested_inner.h` and the guard-skipped edge in `rfb_nested_intn.h` --
// lies inside that realized body, which is B tokens, so none of those
// directives survives into the output.  The re-entry proof used to count them
// as surviving directives and fall back to raw B for the whole file.
typedef long rfb_nested_long;

typedef rfb_nested_long rfb_nested_intn;

int rfb_nested_keep = 1;

int rfb_nested_outer_tail = 0;


int main(void) { return rfb_nested_keep; }
