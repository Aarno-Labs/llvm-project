int foobar(void) { return 7; }
int foo_bar(void) { return 9; }
int main(void) {
  return foo_bar() == 7 ? 0 : 1;
}
