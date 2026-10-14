// RUN: %clang-refold-tester-verify-off header_token_edit_keeps_payload_apart_from_preserved_token
// Regression: B rewrites the header's `int f(s *t);` as `typedef s *p;` and
// `int f(p t);`.  The mapped-header edit replaced the source bytes `s *` with
// B's exact token cover `p`, and the preserved `t` follows `*` with no white
// space, so the output read `int f(pt);`.  Every edit seam is now kept a
// token boundary of the output.
struct s { int v; };
typedef struct s s;
typedef s *p;
int f(p t);
int f(p t) { return t->v; }
