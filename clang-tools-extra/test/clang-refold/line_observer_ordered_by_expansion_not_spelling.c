// RUN: %clang-refold-tester-with-lines line_observer_ordered_by_expansion_not_spelling
// Runs with line directives enabled: the conditional-join repair is gated on
// them, so `--no-lines` would never reach the theorem under test.
// Regression: a suffix observer must be ordered by where it observes, not by
// where its token is spelled.  `__LINE__` inside a replacement list is spelled
// once, in the `#define`, but evaluated at every expansion point.  Recording
// the observer at the definition put every expansion upstream of any boundary
// after it, so the post-conditional `#line` repair -- which requires a known
// observer -- found an empty suffix-observer set and failed closed with
// NoCanonicalSuffixOrder, taking the whole translation unit to a verbatim copy
// of the edited stream.
//
// The conditional group below is edited, so the join repair runs; the observer
// it must order against is the WHERE() call after the group, whose `__LINE__`
// is spelled far earlier.
#define WHERE(v) ((v) + __LINE__)

#ifdef PICK_A
int chosen = 1;
#else
int chosen = 2;
#endif

int after_group = WHERE(10);
int tail = WHERE(20);
