int fail(const char *, int);
int g(int);
int f(int x) {
  return ((g(x) > 0) ? 0 : fail("g(x) > 0", 12));
}
