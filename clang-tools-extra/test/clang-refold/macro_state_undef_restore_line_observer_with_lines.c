// RUN: %clang-refold-tester-with-lines-verify-off macro_state_undef_restore_line_observer_with_lines
// The lines an undef/restore repair adds are resynchronized for a later
// `__LINE__` observer.
//
// The repair writes `#undef` and `#define` lines around the payload, which
// shifts every later physical line by two; the observer must still report the
// line B recorded.
#define LINE_ZZ 3
int line_b = LINE_ZZ;
int line_arr[] = { 1 };
int line_later = LINE_ZZ;
int line_observed = __LINE__;
