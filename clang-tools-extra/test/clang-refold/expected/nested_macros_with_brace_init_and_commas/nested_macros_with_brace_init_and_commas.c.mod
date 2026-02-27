// RUN: %clang-refold-tester-with-lines nested_macros_with_brace_init_and_commas

// test106.c: nested macros with braces/commas.
#define V2(a,b) { (a), (b) }
#define ARR2(x,y) (int[2])V2((x),(y))
#define SUM2(p) ((p)[0] + (p)[1])

int main(void) {
  int *p = ARR2(3, 5);
  return SUM2(p) == 8  ? 0 : 1;
}
