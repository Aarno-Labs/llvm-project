// Fail-closed companion to the payload-insensitivity theorem: the same
// straddling shape, but the edited payload names the poisoned identifier.  Only
// one placement re-preprocesses at all, so the two are not equivalent and the
// alignment still cannot say which is meant.  The closure must refuse rather
// than commit the placement that happens to compile.
//
// This asserts the refusal, not the output.  A refused closure currently costs
// the whole translation unit, because giving up a bounded region instead is
// region-scoped emission and that is unbuilt -- so pinning the emitted bytes
// would encode raw B as the desired result and grow the migration set.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/mixed_tu_include_closure_rejects_sensitive_payload_across_tu_pragma.c
// RUN: cp %S/headers/two.inc %S/headers/two.inc %t.dir/headers/
//
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=%t.dir/map.json mixed_tu_include_closure_rejects_sensitive_payload_across_tu_pragma.c -o %t.dir/a.i
// RUN: diff -u %S/expected/mixed_tu_include_closure_rejects_sensitive_payload_across_tu_pragma/mixed_tu_include_closure_rejects_sensitive_payload_across_tu_pragma.c.i %t.dir/a.i
//
// RUN: cd %t.dir && clang-refold --no-lines --strict --verify-output=fatal --log-level=trace --pp %t.dir/a.i --pp-mod %S/expected/mixed_tu_include_closure_rejects_sensitive_payload_across_tu_pragma/mixed_tu_include_closure_rejects_sensitive_payload_across_tu_pragma.c.i.mod --refold-map %t.dir/map.json --out %t.dir/a.c.mod 2>&1 | FileCheck %s
//
// CHECK: cannot be partitioned around 1 preserved TU pragma(s)
// CHECK-SAME: has no determined side of the preserved directive
// CHECK-SAME: observes its PoisonIdentifiers state
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#include "two.inc"
};
