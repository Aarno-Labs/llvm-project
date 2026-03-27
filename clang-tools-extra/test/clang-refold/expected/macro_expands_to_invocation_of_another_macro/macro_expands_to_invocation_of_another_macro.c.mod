// RUN: %clang-refold-tester-with-lines macro_expands_to_invocation_of_another_macro

// test111.c: macro expands to invocation of another macro (two-stage).
#define M2(x) ((x) + 10)
#define M1(x) M2(x)

int main(void) {
  int v = ((5) + 11);
  return v == 16 ? 0 : 1;
}
