// RUN: %clang-refold-tester-with-lines line_observer_displacement_narrows_to_one_macro
// Terminal-request attribution.  Rewriting CHECK collapses its two-line
// spelling onto one -- admissible, because CHECK spells no line observer -- and
// that moves AT(x) on the same line up with it, so AT's __LINE__ reads one less
// than the edited stream carries.  The assembly check that catches this knows
// only the edited-stream token the two streams parted at, and used to discard
// it: the request named no region and the ladder surrendered the whole
// translation unit to the raw edited stream.  Carrying the token through the
// alignment names __LINE__, then AT, and expanding AT alone realizes the
// observer as a literal.  Everything else -- both definitions, the CHECK
// callsite, this comment -- is preserved.
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, 0))
#define AT(v) ((v) + __LINE__)

int f(int x, int y) {
  return CHECK(q(y) < 1) + ((y) + 20);
}
