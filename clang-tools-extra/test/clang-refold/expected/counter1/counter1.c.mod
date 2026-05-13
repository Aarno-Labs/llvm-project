// RUN: %clang-refold-tester counter1
int foo(const char* fmt, unsigned x) {
  printf(fmt, x);
}

int main() {
  foo("first counter: %u", __COUNTER__);
  foo("second counter: %u", __COUNTER__);
  foo("third counter: %u", __COUNTER__);
  foo("fourth counter: %u", 10);
  foo("fifth counter: %u", 15);
  foo("sixth counter: %u", 5);
  foo("seventh counter: %u", 6);

  return 0;
}
