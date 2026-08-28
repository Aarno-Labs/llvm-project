// RUN: %clang-refold-tester-with-lines line_control_prune_not_blamed_for_prior_divergence
// RUN: FileCheck --input-file=%t/outputs/line_control_prune_not_blamed_for_prior_divergence.out %s
//
// Regression (diagnostic): the final line-control prune must not be named as
// the cause of a divergence it inherited.
//
// The prune re-checks the text it produced, because whatever it deleted was
// never covered by the check that ran before it.  But nothing on this path
// verifies the text it replaced: the driver's own verifier is positioned after
// the engine returns and is skipped once a terminal request exists.  So the
// re-check used to report its finding as damage the prune had done, with no
// evidence either way, and the request it raised sent every reader after the
// wrong stage.
//
// Here the divergence predates the prune.  Rewriting CHECK collapses its
// two-line spelling onto one -- admissible, CHECK spells no line observer --
// which moves AT(x) up with it on the same line, so AT's __LINE__ is already
// wrong in the assembly the prune is handed.  Verifying that text before
// blaming the prune is what distinguishes the two, and it must report the
// prune as innocent.
//
// The `{{y}}` keeps each CHECK-NOT from matching its own text, should a trace
// ever quote the source around it.
//
// CHECK-NOT: prune broke the assembl{{y}}
// CHECK: the pre-prune assembly does not either
// CHECK-SAME: line-control prune is not the cause
// CHECK-NOT: prune broke the assembl{{y}}
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, 0))
#define AT(v) ((v) + __LINE__)

int f(int x, int y) {
  return CHECK(g(x)
               > 0) + AT(x);
}
