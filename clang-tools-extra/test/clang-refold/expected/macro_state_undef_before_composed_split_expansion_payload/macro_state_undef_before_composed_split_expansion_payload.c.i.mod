typedef long M;
long f(long c, long s, long i) {
  c = (!c || i < 0) ? ((M)0)
      : (i < 0 ? (M)0 : c + i) + 1;
  return c;
}
