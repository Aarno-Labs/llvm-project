// RUN: %clang-refold-tester macro_arg_edit_preserves_invocation_comment
#define ID(x) x

int a = ID(/*keep*/ 1);

int main(void) {
  printf("%s:%d\n", __FILE__, __LINE__);
  return a;
}
