// RUN: %clang-refold-tester define_expand_object_payload_conflict
#define BAR 5
int sentinel = FOO;
#define FOO 10
int main(void) {
  return FOO+BAR;
}
