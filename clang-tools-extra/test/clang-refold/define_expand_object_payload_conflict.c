// RUN: %clang-refold-tester define_expand_object_payload_conflict
#define BAR 5
int before = 1;
#define FOO 10
int after = 2;

int main(void) {
  return FOO+BAR;
}
