// RUN: %clang-refold-tester tu_include_closure_refuses_touched_include_with_outliving_pragma
// RUN: FileCheck --input-file=%t/outputs/tu_include_closure_refuses_touched_include_with_outliving_pragma.out %s
//
// Fail-closed regression for a demonstrated wrong-output defect.
//
// A TU/include closure may consume a *touched* include -- one whose tokens the
// hunk covers -- on the strength of realizing its expansion.  That accounts for
// the include's tokens, and through macro-state liveness repair for its macro
// directives.  It does not account for pragma state the header establishes that
// outlives the include.  Here the header's only recorded structure is
// `#pragma push_macro`, and the *translation unit* pops it after the include has
// ended, so the pop observes state the include established.
//
// Deleting the include therefore deletes the push while the pop survives, and
// the pop then restores nothing.  Before the touched-include check this closure
// was accepted and the emitted source preprocessed to `int push_only_outer =
// PUSH_ONLY_M;` where B requires `1` -- wrong source, emitted silently at the
// default `--verify-output=off` because only the closing check would have
// caught it.
//
// The refusal below is still the point of this test, and the CHECK lines still
// pin it.  What changed is what the refusal now costs: nothing.  The closure is
// refused, the attempt asks for the terminal carrier, and the ladder moves the
// straddling deletion run back onto the header's own cover -- so the include
// materializes, the pragma survives where it was written, and no source
// structure is given up.  The expected `.c.mod` is that refold, not the raw
// edited preprocessed stream it used to pin.
//
// CHECK: TU/include closure rejected
// CHECK-SAME: consume touched include
// CHECK-SAME: subtree owns a pragma outliving it
#define PUSH_ONLY_M 1
#include "push_macro_only_carrier.h"
#undef PUSH_ONLY_M
#define PUSH_ONLY_M 2
int push_only_inner = PUSH_ONLY_M;
#pragma pop_macro("PUSH_ONLY_M")
int push_only_outer = PUSH_ONLY_M;
