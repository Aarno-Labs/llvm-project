// RUN: %clang-refold-tester-with-lines line_resync_injects_before_reindented_carried_prefix
// A newline-drifting edit must inject its `#line` resync before the preserved
// observer it displaces, even when the edited stream respells the carried
// prefix's indentation.
//
// The accepted insertion anchor lands inside `int r`, so the replacement ends
// in a carried copy of the original line prefix -- but spelled with the edited
// stream's indentation (a space) rather than the original's (a tab).  Requiring
// that carried prefix to match byte-for-byte refused the injection point, and
// the resync could then only be deferred to the next beginning-of-line, which
// is already past `CHECK(a)`.  The deferred directive is redundant there, so
// the final minimizer proves it removable and deletes it, leaving `__LINE__`
// one line too high with no directive at all.
//
// Companion to line_resync_precedes_header_macro_line_observer.c, which covers
// the indentation-only trailing run.
#define CHECK(x) ((x), __LINE__)

static int probe(int a)
{
	int t = 0;
#line 21 "line_resync_injects_before_reindented_carried_prefix.c"
 int r = CHECK(a);
	return r;
}
