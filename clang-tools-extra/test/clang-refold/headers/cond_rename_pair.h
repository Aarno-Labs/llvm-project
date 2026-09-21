#define HAS_FAST 1

/* Fast path when available. */
static int first(int d) {
#ifdef HAS_FAST
  return d - 1; /* fast */
#else
  return 13;
#endif
}

/* Second helper. */
static int second(int d) {
  return d;
}
