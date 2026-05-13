// RUN: %clang-refold-tester define_expand_function_payload_conflict
#define BAR(x) ((x)*2)
int sentinel = FOO(9);
int main(void) {
  return ((3)+1) + BAR(4);
}
