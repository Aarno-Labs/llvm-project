#line 1 "smallest_expansion.c"
// RUN: %clang-refold-tester-with-lines smallest_expansion LINES
// RUN: %clang-refold-tester smallest_expansion NOLINES
#define FOO(x) ((x) * (x) + 2)
int x[] = {
4
};
#line 8 "smallest_expansion.c"

int main() {
  printf("%s:%d\n", __FILE__, __LINE__);
  return FOO(6);
}
