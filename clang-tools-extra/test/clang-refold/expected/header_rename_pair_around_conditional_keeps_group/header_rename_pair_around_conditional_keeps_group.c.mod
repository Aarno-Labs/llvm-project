// RUN: %clang-refold-tester header_rename_pair_around_conditional_keeps_group
// Regression: renaming two adjacent header functions must not rewrite the
// first one from the edited stream.  The rename hunks bracket the first
// function's `#ifdef` group without touching any token of it, but the
// coalescer for consumed conditional groups merged them, because the selected
// arm lay inside their combined range.  The group, its inactive `#else` arm and
// both comments were then re-spelled from B and dropped.  Coalescing may absorb
// only unedited tokens of the arm it consumes.
#define HAS_FAST 1

/* Fast path when available. */
static int first_x(int d) {
#ifdef HAS_FAST
  return d - 1; /* fast */
#else
  return 13;
#endif
}

/* Second helper. */
static int second_x(int d) {
  return d;
}

int use(int d) { return first_x(d) + second_x(d); }
