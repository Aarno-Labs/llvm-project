// RUN: %clang-refold-tester theorem_counter_state_stability_declared
int foo(const char* fmt, unsigned x) {
  printf(fmt, x);
}

int main() {
  foo("first counter: %u", __COUNTER__);
  foo("second counter: %u", __COUNTER__);
  foo("third counter: %u", __COUNTER__);
  foo("fourth counter: %u", __COUNTER__);
  foo("fifth counter: %u", __COUNTER__);
  foo("sixth counter: %u", __COUNTER__);
  foo("seventh counter: %u", __COUNTER__);

  return 0;
}
