// RUN: %clang-refold-tester-verify-off materialized_header_macro_state_undef_instead_of_carry_across_indirect_read
// Regression: an include-owned macro patch whose B payload spells `VAL` may
// have `#define VAL` carried past it only when no preserved header source the
// definition crosses needs it.  `crosser` reads `VAL` through `IND` without
// spelling it, so carrying the definition made `crosser` return the enumerator
// 3 where B has 99.  The crossing check scanned text alone; it now also asks
// whether the producer recorded an expansion of `VAL` there, so the carry is
// refused and `VAL` is undefined around the payload's line instead.  The
// enumerator keeps the edited stream valid C.
enum { VAL = 3 };
// Header for materialized_header_macro_state_undef_instead_of_carry_across_indirect_read.
//
// `crosser` lies between `#define VAL` and the edited invocation and reads
// `VAL` through `IND` without spelling it.
#define IND VAL
#define ID(x) (x)
#define VAL 99
int crosser(void) { return IND; }
#undef VAL
int patched(void) { return (VAL); }
#define VAL 99
int tu(void) { return 0; }
