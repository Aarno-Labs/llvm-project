// RUN: %clang-refold-tester-with-lines line_resync_before_observer_when_edit_starts_after_indent
// A line-deleting edit must be able to place its `#line` resync at the front of
// the replacement even when the edit begins after the line's indentation.
//
// The aligner anchors this edit one byte past the line start, because the
// leading tab is copied verbatim ahead of the replacement.  The replacement
// carries the following line's prefix, so the directive belongs at the front of
// the replacement -- but requiring a strict beginning-of-line there refused the
// placement, and the repair could then only be deferred to the next safe
// boundary, which is already past `CHECK(a)`.  With the drift undischarged the
// callsite moved up one physical line and `__LINE__` expanded one too low.
//
// Horizontal whitespace before `#` still introduces a directive, so the copied
// indentation does not prevent the resync from starting a logical line.
#define CHECK(x) ((x), __LINE__)

static int probe(int a)
{
	int p = 1;
	int q = 2; int r = CHECK(a);
	return r;
}
