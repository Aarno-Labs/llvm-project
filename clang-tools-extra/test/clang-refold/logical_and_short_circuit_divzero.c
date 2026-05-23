// RUN: %clang-refold-tester-with-lines logical_and_short_circuit_divzero
#define DEAD_LINE 9001
#define LIVE_LINE 700

#if 0 && (1 / 0)
#  define LINE_TOKEN DEAD_LINE
#  line LINE_TOKEN "dead-short-circuit.c"
int selected = __LINE__;
#else
#  define LINE_TOKEN LIVE_LINE
#  line LINE_TOKEN "live-short-circuit.c"
int selected = __LINE__;
#endif
int suffix = __LINE__;
