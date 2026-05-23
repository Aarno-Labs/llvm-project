#line 1 "line_filename_hex_escape_resync.c"
// RUN: %clang-refold-tester-with-lines line_filename_hex_escape_resync
#line 900 "logical\x5fhex.c"
int inserted = 0;
#line 900 "logical_hex.c"
const char *file = __FILE__;
