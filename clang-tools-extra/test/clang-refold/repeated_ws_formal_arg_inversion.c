// RUN: %clang-refold-tester repeated_ws_formal_arg_inversion
// Regression (was a miscompile): a formal repeated with whitespace-only
// separation in the body (`x x`) must invert to a single argument. Editing the
// expansion 1 1 -> 9 9 refolds to D2(9); the tool previously emitted D2(9 9)
// (which re-expands to 9 9 9 9) because the producer merged both formal
// occurrences into one argument span.
#define D2(x) x x
int a[] = {D2(1)};
