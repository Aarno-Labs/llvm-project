int foobar(void) { return 40; }
int main(void) {
  return foobar() == 40 && foobar() == 40 ? 0 : 1;
}
