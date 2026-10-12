// RUN: %clang-refold-tester-verify-off payload_opening_call_after_name_before_whole_cover_is_undefined
// Regression: the spelled `F` is not called in A, because the token after it in
// source is `M`, although M expands to text beginning with `(`.  B edits M's
// replacement-list `1`, which only a whole cover realizes, and the cover's B
// text then begins with the `(` right after `F`.  The repair must undefine `F`
// before the line so the cover is not read as F's arguments.
int F(int);
#define F(v) v
#define M(a) (a + 1)
void g(void) { (void)F M(2); }
