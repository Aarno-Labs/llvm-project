// RUN: %clang-refold-tester-with-lines-verify-off line_observer_displacement_narrows_with_verification_off
// Companion to line_observer_displacement_narrows_to_one_macro, run with output
// verification off -- the tool's own default.  With no closing verifier the
// final line-control prune is never re-checked, and the displaced observer is
// caught instead by the line-observer audit, which is skipped whenever an
// earlier check already raised a request.  That audit indexes the edited stream
// itself, so it names the owning region directly rather than through the
// verifier-numbering reconciliation the ladder needs.  The same one expanded
// macro, and the same preserved source, must come out.
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, 0))
#define AT(v) ((v) + __LINE__)

int f(int x, int y) {
  return CHECK(q(y) < 1) + ((y) + 18);
}
