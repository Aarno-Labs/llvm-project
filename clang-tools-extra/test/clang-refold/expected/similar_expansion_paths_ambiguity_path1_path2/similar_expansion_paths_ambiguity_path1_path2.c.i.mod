int foobar(void) { return 40; }
int main(void) {
  return foobar() == 40 && foobaz() == 40 ? 0 : 1;
}
