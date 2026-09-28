// RUN: %clang-refold-tester-with-lines-verify-off header_line_without_filename_keeps_header_file_observer
//
// A header's own `#line` re-establishes the line when the header is inlined,
// but a `#line` without a filename operand keeps the presumed file, and after
// inlining that file is the includer's.  The header's `__FILE__` therefore
// still needs the include-entry `#line` naming the header.
//
// Any executed directive ahead of the observer used to discharge the wrapper,
// so the edit inside the header inlined it bare and `__FILE__` expanded to this
// file's name.  At --verify-output=fatal the refold was refused instead.
#line 1 "headers/h_line_nofile_file_observer.h"
#line 80
const char *file = __FILE__;
int value = 2;
int tail = 3;
