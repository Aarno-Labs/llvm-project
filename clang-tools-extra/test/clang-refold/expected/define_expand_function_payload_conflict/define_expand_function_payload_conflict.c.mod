// RUN: %clang-refold-tester define_expand_function_payload_conflict
#define BAR(x) ((x)*2)
int sentinel = FOO(9);
#define FOO(x) ((x)+1)
int main(void) {
  return FOO(3) + BAR(4);
}
