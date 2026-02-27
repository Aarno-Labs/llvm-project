// RUN: %clang-refold-tester-with-lines token_paste_declare_array_style

// test66: DECLARE_ARRAY-style token paste.
#include <stddef.h>
#define DECLARE_ARRAY(T) \
  typedef struct { size_t n; T *data; } array_##T##_t; \
  array_##T##_t array_##T##_create(T *p, size_t n);
DECLARE_ARRAY(int)
int main() { return 0; }
