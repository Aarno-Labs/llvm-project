// RUN: %clang-refold-tester define_expand_object_payload_conflict
#define BAR 5
int sentinel = FOO;
int main(void) {
  return 10+BAR;
}
