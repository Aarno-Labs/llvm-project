// RUN: %clang-refold-tester-relaxed non_strict_stringify_macro_arg_edit
//
// Non-strict (relaxed) coverage.
//
// The harness runs clang-refold with --strict everywhere else, so the default,
// non-strict refold pipeline -- the mode real invocations use unless they opt
// into --strict -- is otherwise never exercised by a refold-step regression.
// This test drives it via %clang-refold-tester-relaxed (refold without
// --strict).
//
// The edit rewrites a stringified macro argument (STRINGIFY(alpha) -> "beta").
// In relaxed mode RefoldMacroReplay accepts the args-only replay without the
// strict stringify-span validation, and the theorem audit does not enforce
// terminal fallback.  The relaxed pipeline must still reconstruct a sound
// source edit that preserves the macro invocation, which the verifier confirms
// by re-preprocessing the refolded source.  (For this fully provable edit the
// result matches what strict would produce; the point is to keep the relaxed
// code path regression-visible.)
#define STRINGIFY(x) #x
const char *name = STRINGIFY(alpha);
