int fail(const char *, int);
int g(int);
int f(int x) {
  return ((g_r0(x) > 0) ? 0 : fail("g_r0(x) > 0", 16));
}
