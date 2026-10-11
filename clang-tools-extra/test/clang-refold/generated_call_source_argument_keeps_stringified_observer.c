// RUN: %clang-refold-tester-verify-off generated_call_source_argument_keeps_stringified_observer
// A generated call's source-spelled actual is rewritten in place only when
// every occurrence of its formal replays B.  `GET(ADD)` yields `ADD`, which
// reads `(10)` from the source, and ADD's `a` reaches the output twice: as
// `10` and stringified as `"10"`.  B edits only the plain copy, which no
// spelling of `a` reproduces, so the callsite is expanded.  Rewriting the
// source bytes of `10` by their token map alone emitted `GET(ADD)(11 )`, which
// preprocesses to `11 + sizeof "11"`.
#define ADD(a) a + sizeof #a
#define GET(f) f
int k = GET(ADD)(10);
