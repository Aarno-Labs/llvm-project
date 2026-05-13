// RUN: %clang-refold-tester define_preserve_object_like
#define ANSWER 42
int main(void) {
  return ANSWER;
}
