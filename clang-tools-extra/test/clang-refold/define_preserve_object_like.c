// RUN: %clang-refold-tester define_preserve_object_like
int before = 1;
#define ANSWER 42
int after = 2;

int main(void) {
  return ANSWER;
}
