// RUN: %clang-refold-tester-with-lines line_resync_before_observer_when_edit_starts_after_indent
// RUN: FileCheck %s --check-prefix=AUDIT \
// RUN:   < %t/outputs/line_resync_before_observer_when_edit_starts_after_indent.out
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

// The post-assembly observer audit must actually reach this observer and clear
// it.  Asserting a non-zero observer count is the guard that matters: the audit
// silently degrades to a no-op if the `__LINE__` spans stop being carried
// across the A/B alignment, and a no-op audit still reports success.
// AUDIT: line/observer-audit{{.*}}observers={{[1-9][0-9]*}}{{.*}}carries='22'{{.*}}clean
