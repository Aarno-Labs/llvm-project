// RUN: %clang-refold-tester-with-lines line_min_refolded_source_idempotence FIRST
#line 1 "headers/line_min_refolded_source_idempotence.h"
int header_value = 1;
#line 20 "line_min_refolded_source_idempotence.c"
int main(void) { return 0; }
