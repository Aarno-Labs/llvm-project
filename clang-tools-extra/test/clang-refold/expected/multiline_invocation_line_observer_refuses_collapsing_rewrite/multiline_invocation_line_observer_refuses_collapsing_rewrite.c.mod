// RUN: %clang-refold-tester-with-lines multiline_invocation_line_observer_refuses_collapsing_rewrite
// Fail-closed floor for the multi-line invocation line collapse.  The edit
// replaces the whole actual, so the new spelling shares no prefix or suffix
// token with the recorded one and cannot be spliced into it; the callsite
// rewrite would span one physical line where the recorded spelling spans two.
// __LINE__ expands inside this invocation and takes the line of its closing
// paren, so that rewrite is not realizable and must be refused.  Refusing costs
// this one expanded invocation; admitting it costs the whole translation unit,
// which used to fall back to the raw edited stream.
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, __LINE__))

int f(int x, int y) {
  return ((q(y) < 1) ? 0 : fail("q(y) < 1", 17));
}
