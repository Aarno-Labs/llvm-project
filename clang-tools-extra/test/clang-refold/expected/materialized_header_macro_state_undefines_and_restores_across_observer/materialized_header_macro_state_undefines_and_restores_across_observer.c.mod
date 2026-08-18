// RUN: %clang-refold-tester materialized_header_macro_state_undefines_and_restores_across_observer
// RUN: FileCheck --input-file=%t/outputs/materialized_header_macro_state_undefines_and_restores_across_observer.out %s
//
// Regression for the second macro-state repair available to an include-owned
// macro patch.
//
// The edit replaces the actual of an include-owned `ID(...)` invocation with
// the identifier `VAL`, which is also the name of a header macro defined
// earlier in the same header.  Materializing the header has to emit that
// replacement under a macro environment where `VAL` is *not* active.
//
// The repair that existed was to carry `#define VAL` past the replacement, and
// it cannot apply here: the header source in between -- `crosser` -- observes
// `VAL`, so moving the definition later would change how that preserved source
// preprocesses.  Refusing there surrendered the whole translation unit.
//
// The second repair leaves every definition exactly where it is, undefines the
// observed name immediately before the replacement's own line, and restores it
// immediately after from the producer's directive text.  The restore is what
// makes the repair self-contained: macro state after the repaired interval is
// identical to the state before it, so nothing is required of the header suffix
// or of translation-unit source after the include -- neither of which this
// proof can see.  Only the repaired line changes what it observes, and its
// prefix and suffix are each proven indifferent to the name.
//
// `crosser` therefore still sees `VAL` defined and still preprocesses to 99,
// while the replacement sees it undefined and stays the literal token `VAL`.
//
// CHECK: undefines 'VAL'
// CHECK-SAME: restores it immediately after
// Header for the include-owned macro-state repairs.
//
// `VAL` is defined first and observed by `crosser` before the invocation the
// edit lands on, so `#define VAL` cannot be carried past the replacement.  The
// repair undefines `VAL` around the replacement's line and restores it after.
#define VAL 99
int crosser(void) { return VAL; }
#define ID(x) (x)
#undef VAL
int patched(void) { return (VAL); }
#define VAL 99
int tu(void) { return 0; }
