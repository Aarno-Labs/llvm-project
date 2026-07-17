// RUN: %clang-refold-tester-with-lines stress_stringify_and_standard_same_formal
struct Pair { const char *s; int v; };
#define PAIR(x) { #x, (x) }

struct Pair p = PAIR(22);
