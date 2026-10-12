// RUN: %clang-refold-tester-verify-off whole_cover_consumed_source_macro_name_in_b_is_undefined_and_restored
// Regression: a whole cover's replacement is the cover's own B text, so the `X`
// that B spells where ADD read `2` past CALL's closing parenthesis is an
// identifier that must not expand.  The macro-name-in-B repair must reach the
// cover as it reaches a mapped edit: undefine `X` before the replacement's line
// and restore it after, since the suffix still reads it.  B edits CALL's
// replacement-list `int`, which only an expansion realizes.  The enumerator
// keeps the edited stream valid C.
enum { X = 3 };
#define X 5
#define ADD(a, b) a + b
#define CALL(f, a) int k = f(a
CALL(ADD, 1), 2);
int m = X;
