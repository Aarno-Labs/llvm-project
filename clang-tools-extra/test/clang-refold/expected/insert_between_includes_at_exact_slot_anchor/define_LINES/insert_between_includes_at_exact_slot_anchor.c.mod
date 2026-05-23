// RUN: %clang-refold-tester-with-lines insert_between_includes_at_exact_slot_anchor LINES
// RUN: %clang-refold-tester insert_between_includes_at_exact_slot_anchor NOLINES
const char *s =
#include "left_string.h"
"-"
#line 5 "insert_between_includes_at_exact_slot_anchor.c"
#include "right_string.h"
;

int main(void) {
  printf("%s:%d\n", __FILE__, __LINE__);
  return s[0];
}
