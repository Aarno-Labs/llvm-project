// RUN: %clang-refold-tester alignment_window_structure_repair_keeps_gap_bytes_around_inserted_lines
// RUN: FileCheck --input-file=%t/outputs/alignment_window_structure_repair_keeps_gap_bytes_around_inserted_lines.out %s
//
// Regression: when an inserted block of whole lines can slide across a
// repeated line, the structure-respecting repair keeps the map under which B
// holds A's bytes between every pair of matched tokens, with the insertion at
// one point.
//
// B inserts `helper` after `finish`, before the empty `PRIVATE` invocation's
// printed indentation.  The core-forced map replaces `; } char *` around that
// invocation, which no owner covers, so declining requests terminal fallback.
// Two optimal maps insert whole lines of B: one matches `finish`'s `}`, the
// other matches `helper`'s and inserts `}` through `helper`'s last statement.
// The second changes the bytes between that `}` and `char`, from A's line
// break and indentation to B's blank line; the first keeps them.  Before the
// byte key the repair kept both maps, they realized two accepted outputs, and
// the run reached the whole-translation-unit carrier.
//
// The deleted `basis` declarations, whose core-forced map widens through the
// first `PRIVATE`, make the legacy boundary proposal's own realization request
// terminal fallback, so the window reaches the repair rather than that rule.
//
// CHECK: keeps 1 of its 10 optimal map(s) under the structure-respecting keys
// CHECK: window 0 committing structure-respecting repair: declining requests terminal fallback
#define PRIVATE
struct config { int x; };

PRIVATE struct config *newconfig(void) { return 0; }

static int finish(int *fp)
{
  fp[0] = 0;
  return fp[1];
}

/* Search for the name. */
        static int helper(const char *base, int c) {
    const char *result = base + c;
    if (!result) return -1;
    return (int)(result - base);
}

PRIVATE char *pathsearch(char *argv0)
{
  return argv0;
}
