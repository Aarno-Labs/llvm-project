// RUN: %clang-refold-tester macro_state_nested_argument_callsite_envelope_piece

// ID(1) is written inside SUM's argument, so its callsite bytes nest inside
// SUM's.  Both are genuine source occurrences and both are proved fully
// consumed, but the source-envelope normalizer had no macro-in-macro nesting
// rule and rejected the pair as unorderable.  The inner callsite is interior to
// the outer expansion -- its PP cover nests in the outer's -- so the outer
// piece absorbs it.
int untouched = 1;

#define ID(x) (x)
#define SUM(a, b) ((a) + (b))
int before = 10, r = 0, after = 20;
