// RUN: %clang-refold-tester-with-lines wide_universal_char_escape_value
#define GOOD_LINE 700
#define BAD_LINE 9001

#if L'\u0100' == 256
#  define LINE_TOKEN GOOD_LINE
#  line LINE_TOKEN "wide-true.c"
int selected = __LINE__;
#else
#  define LINE_TOKEN BAD_LINE
#  line LINE_TOKEN "wide-false.c"
int selected = __LINE__;
#endif
int selected_extra = 701;
#line 706 "wide-true.c"
int suffix = __LINE__;
