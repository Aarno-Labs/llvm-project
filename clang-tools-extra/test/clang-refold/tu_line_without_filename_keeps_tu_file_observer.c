#line 10
// RUN: %clang-refold-tester-with-lines-verify-off tu_line_without_filename_keeps_tu_file_observer
//
// A `#line` without a filename operand keeps the presumed file, and at the top
// of the translation unit that is its physical name.  The refolded source is
// replayed under a different one, so `__FILE__` below still needs the synthetic
// TU prologue naming this file even though the output already begins with a
// `#line`.  The directive must stay the file's first line to show that.
//
// The prologue used to be skipped whenever the output began with `#line`, so
// this refold emitted `__FILE__` bare and it expanded to the output path.  At
// --verify-output=fatal the refold was refused instead.
const char *file = __FILE__;
int tail = 1;
