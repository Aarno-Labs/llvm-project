// RUN: %clang-refold-tester materialized_header_macro_state_rejects_carry_across_observer
// RUN: FileCheck --input-file=%t/outputs/materialized_header_macro_state_rejects_carry_across_observer.out %s
//
// Regression: first coverage for the MacroStateStabilizable terminal
// obligation, which the corpus reaches constantly and no in-tree test reached.
//
// The edit replaces the actual of an include-owned `ID(...)` invocation with
// the identifier `VAL`, which is also the name of a header macro defined
// earlier in the same header.  Materializing the header therefore has to emit
// the B replacement under a macro environment where `VAL` is *not* active,
// which means carrying `#define VAL` past the replacement.  It cannot: the
// header source in between -- `crosser` -- observes `VAL`, so moving the
// definition later would change how that preserved source preprocesses.
//
// The proof correctly refuses.  Note what the request carries: an owner id and
// a source range, not only a state-component name, so a future partial
// realization has a region to work from rather than the whole file.
//
// The emitted stream deliberately names `VAL` at a point where no definition
// survives; that is the state conflict under test, and the harness oracle
// re-preprocesses rather than compiles.
//
// PINS BEHAVIOR THAT IS INTENDED TO BE REMOVED.  The expected `.c.mod` for this
// test is the raw edited preprocessed stream: no `#include`, no `#define VAL`,
// no `#define ID`.  That is what this obligation emits today, recorded so that
// replacing raw-B emission with a partial realization shows up as a reviewed
// expectation change rather than a silent one.  When that lands, this test
// should preserve both definitions and the header body and give up only the
// region whose macro state could not be stabilized, and the CHECK lines below
// should assert that region instead of `terminalFallback=yes(raw-B)`.
//
// Anchor on the request record itself: an earlier witness line carries the same
// obligation but truncates its detail, so it cannot satisfy the CHECK-SAME run.
// CHECK: terminal fallback requested: action=raw-b-emission obligation=MacroStateStabilizable
// CHECK-SAME: reason=MacroStateNotStabilizable
// CHECK-SAME: stage=include/materialized-macro-state
// CHECK-SAME: cannot carry
// CHECK: terminalFallback=yes(raw-B)
#include "materialized_header_macro_state_observer.h"
int tu(void) { return 0; }
