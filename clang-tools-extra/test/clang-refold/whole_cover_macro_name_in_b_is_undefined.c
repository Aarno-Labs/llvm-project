// RUN: %clang-refold-tester-verify-off whole_cover_macro_name_in_b_is_undefined
// Regression: a whole cover's replacement is the cover's own B text, so the `X`
// that B spells is an identifier that must not expand.  The macro-name-in-B
// repair must reach the cover as it reaches a mapped edit: undefine `X` before
// the replacement's line.  B edits F's replacement-list `1`, which only an
// expansion realizes.  The enumerator keeps the edited stream valid C.
enum { X = 3 };
#define X 5
#define F(a) int k = a + 1;
F(2)
