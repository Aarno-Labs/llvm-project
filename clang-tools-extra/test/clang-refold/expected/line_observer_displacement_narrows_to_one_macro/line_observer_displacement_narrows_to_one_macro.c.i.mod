int fail(const char *, int);
int g(int), q(int);
int f(int x, int y) {
  return ((q(y) < 1) ? 0 : fail("q(y) < 1", 0)) + ((y) + 20);
}
