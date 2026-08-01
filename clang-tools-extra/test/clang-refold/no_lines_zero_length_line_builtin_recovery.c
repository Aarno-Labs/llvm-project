// RUN: %clang-refold-tester no_lines_zero_length_line_builtin_recovery
//
// Regression: exercise the --no-lines zero-length builtin recovery path.
//
// `VAL` is an object-like macro whose body is `__LINE__`.  When it is expanded
// through `ID(VAL)`, the producer records the resulting numeric token under the
// caller macro body span, leaving the `__LINE__` event's own span zero-length.
// Recovering the ignore-mask bit therefore requires the caller-body anchored
// zero-length recovery in RefoldNoLinesPruning, not the direct-span path.
//
// The edit inserts a declaration above the `ID(VAL)` use, shifting its physical
// line by one.  Re-preprocessing the refolded source yields a different
// `__LINE__` value (19) than the modified stream carries (18).  The verifier
// must prove that difference is an ignorable builtin-line observation rather
// than a dropped edit; the zero-length recovery is load-bearing for that proof.
#define VAL __LINE__
#define ID(x) x
int v = ID(VAL);
