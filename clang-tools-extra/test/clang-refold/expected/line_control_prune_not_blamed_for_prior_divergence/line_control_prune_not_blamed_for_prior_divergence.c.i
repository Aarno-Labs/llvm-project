int fail(const char *, int);
int g(int), q(int);
int f(int x, int y) {
  return ((g(x) > 0) ? 0 : fail("g(x) > 0", 0)) + ((x) + 37);
}
