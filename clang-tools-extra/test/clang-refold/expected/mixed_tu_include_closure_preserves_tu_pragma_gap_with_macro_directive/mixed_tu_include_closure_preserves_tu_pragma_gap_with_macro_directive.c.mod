// Regression for a preserved owner gap holding more than one kind of directive.
//
// The gap between the closure's required source pieces holds a `#pragma` and a
// `#define`. Only pragmas and zero-token includes were ever submitted to the
// shared source-gap theorem as preserved pieces, so the `#define` came back as
// an unowned protected interval and the whole closure was refused -- which
// surrenders the translation unit and drops every directive in it, including
// the two this refusal existed to protect. Refusing the *consumption* of a
// macro-state transition is right; refusing the closure is the same backwards
// trade the payload/preserved-source partition was built to stop making.
//
// A macro directive is preserved on the same terms as a pragma: it is
// tokenless, it is meaningful source, and its position carries state. Both
// directives now survive where they were written, in source order.
//
// A payload straddling the gap is a separate question this does not answer. A
// zone naming a macro the preserved directive binds preprocesses differently on
// either side -- a `#define` would force it before, since the name must survive
// unexpanded to appear in B at all, and an `#undef` would force it after -- and
// deciding that needs the definition's liveness at the split. Such a zone still
// refuses. Here the hunk contributes no B tokens at all, so no zone arises.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive.c
// RUN: cp %S/headers/two_seven.inc %S/headers/two.inc %t.dir/headers/
//
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=%t.dir/map.json mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive.c -o %t.dir/a.i
// RUN: diff -u %S/expected/mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive/mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive.c.i %t.dir/a.i
//
// RUN: cd %t.dir && clang-refold --no-lines --strict --verify-output=fatal --log-level=trace --pp %t.dir/a.i --pp-mod %S/expected/mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive/mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive.c.i.mod --refold-map %t.dir/map.json --out %t.dir/a.c.mod 2>&1 | FileCheck %s
//
// Both directives are preserved, and the gap theorem accounts for both.  Two
// CHECK lines require two occurrences; the byte offsets are deliberately not
// asserted, because they move with this comment block and would make the test
// depend on its own length rather than on the behaviour.
// CHECK: preserving TU directive
// CHECK: preserving TU directive
//
// RUN: diff -u %S/expected/mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive/mixed_tu_include_closure_preserves_tu_pragma_gap_with_macro_directive.c.mod %t.dir/a.c.mod
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 
#pragma GCC poison FOO
#define GAP_VALUE 99
7
};
