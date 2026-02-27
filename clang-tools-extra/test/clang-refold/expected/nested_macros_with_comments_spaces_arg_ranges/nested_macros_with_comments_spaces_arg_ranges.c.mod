// RUN: %clang-refold-tester-with-lines nested_macros_with_comments_spaces_arg_ranges

// test110.c: nesting with comments/spaces that can stress arg range logic.
#define ID(x) x
#define WRAP1(x) ID(x)
#define WRAP2(x) WRAP1( x )
#define WRAP3(x) WRAP2(/*c*/x/*d*/)

int main(void) {
  int v = WRAP3((1 + 3));
  return v == 4  ? 0 : 1;
}
