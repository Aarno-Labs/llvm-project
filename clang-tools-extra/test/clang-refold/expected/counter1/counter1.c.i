
int foo(const char* fmt, unsigned x) {
  printf(fmt, x);
}
int main() {
  foo("first counter: %u", 0);
  foo("second counter: %u", 1);
  foo("third counter: %u", 2);
  foo("fourth counter: %u", 3);
  foo("fifth counter: %u", 4);
  foo("sixth counter: %u", 5);
  foo("seventh counter: %u", 6);
  return 0;
}
