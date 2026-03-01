// RUN: %clang-refold-tester-with-lines token_paste_multiple_identifiers_per_invocation

// test67: Multiple pasted identifiers per invocation; ensures paste gate covers all.
//#include <stddef.h>
#define DECLARE_ARRAY2(T) \
  typedef struct { size_t n; T *data; } arr_##T##_t; \
  int arr_##T##_push(arr_##T##_t *a, T v); \
  T arr_##T##_pop(arr_##T##_t *a);
DECLARE_ARRAY2(int)
int main() { return 0; }
