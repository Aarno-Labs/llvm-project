// Fail-closed regression for the payload/preserved-source partition: the owner
// gap holds a second directive beside the pragma.  Only the pragma is submitted
// as a preserved piece, so the shared source-gap theorem reports the `#define`
// as an unowned protected interval and the closure is refused.  A partition
// that emitted only the pragma would silently drop the macro-state transition.
//
// This asserts the refusal, not the output.  A refused closure currently costs
// the whole translation unit, because giving up a bounded region instead is
// region-scoped emission and that is unbuilt -- so pinning the emitted bytes
// would encode raw B as the desired result and grow the migration set.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/mixed_tu_include_closure_rejects_tu_pragma_gap_with_macro_directive.c
// RUN: cp %S/headers/two_seven.inc %S/headers/two.inc %t.dir/headers/
//
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=%t.dir/map.json mixed_tu_include_closure_rejects_tu_pragma_gap_with_macro_directive.c -o %t.dir/a.i
// RUN: diff -u %S/expected/mixed_tu_include_closure_rejects_tu_pragma_gap_with_macro_directive/mixed_tu_include_closure_rejects_tu_pragma_gap_with_macro_directive.c.i %t.dir/a.i
//
// RUN: cd %t.dir && clang-refold --no-lines --strict --verify-output=fatal --log-level=trace --pp %t.dir/a.i --pp-mod %S/expected/mixed_tu_include_closure_rejects_tu_pragma_gap_with_macro_directive/mixed_tu_include_closure_rejects_tu_pragma_gap_with_macro_directive.c.i.mod --refold-map %t.dir/map.json --out %t.dir/a.c.mod 2>&1 | FileCheck %s
//
// CHECK: rejected position-preserved TU pragma gap
// CHECK-SAME: kind=MacroDefine
// CHECK-SAME: has no caller-authorized source-gap owner
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 1,
#pragma GCC poison FOO
#define GAP_VALUE 99
#include "two_seven.inc"
};
