// RUN: %clang-refold-tester-with-lines-verify-off displaced_line_observer_records_one_terminal_request
// RUN: FileCheck --input-file=%t/outputs/displaced_line_observer_records_one_terminal_request.out %s
//
// Regression (census accuracy): one displaced line observer must record one
// terminal request.
//
// The line-observer audit records the request that names the displacement --
// the diverging edited-stream token and the region owning it -- and then
// returns false.  Its caller performed the fallback and recorded a *second*
// request for the same failure, carrying neither.  A single displaced observer
// was therefore censused as two, one of them unattributable, which reads as an
// independent second problem that the ladder cannot narrow.  The request is
// still mandatory -- an assembly that reaches the seam unnamed ships the raw
// edited stream with a zero exit -- so the caller now asserts the audit
// recorded it instead of raising its own.
//
// Verification is off here because that is the configuration in which this
// audit is the check that fires: whenever the closing verifier exists, the
// prune re-check reaches the seam first and this audit is skipped.
//
// The regex keeps each CHECK-NOT from matching its own text, should a trace
// ever quote the source around it.
//
// CHECK-NOT: terminalFailures={{[2-9]}}
// CHECK: terminalFailures=1
// CHECK-NOT: terminalFailures={{[2-9]}}
int fail(const char *, int);
int g(int), q(int);

#define CHECK(e) ((e) ? 0 : fail(#e, 0))
#define AT(v) ((v) + __LINE__)

int f(int x, int y) {
  return CHECK(g(x)
               > 0) + AT(x);
}
