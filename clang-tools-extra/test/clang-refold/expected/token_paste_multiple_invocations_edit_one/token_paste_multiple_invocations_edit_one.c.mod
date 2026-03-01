// RUN: %clang-refold-tester-with-lines token_paste_multiple_invocations_edit_one

// test68: Multiple invocations; only edit one set of pasted identifiers in B.
//#include <stddef.h>
#define DECLARE_ARRAY3(T) \
  typedef struct { size_t n; T *data; } vec_##T##_t; \
  vec_##T##_t vec_##T##_create(T *p, size_t n);
DECLARE_ARRAY3(float)
DECLARE_ARRAY3(short)
int main() { return 0; }
