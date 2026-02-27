int foobar(void) { return 7; }
int foo_bar(void) { return 9; }
int main(void) {
  return foobar() == 7 ? 0 : 1;
}
