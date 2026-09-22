// RUN: %clang-refold-tester alignment_window_forced_anchors_keep_gap_comment
//
// The K&R-to-prototype rewrite leaves six core-optimal maps for the window
// from `)` of the declaration to `{`, and no commit rule picks one, so the
// window keeps only its forced anchors and becomes one hunk.  The comment lies
// in the A gap between that hunk's `;` and `int`, which no optimal map needs
// replaced.  The hunk is split at that gap, at the latest admissible B
// frontier, so the comment survives directly before the definition.
extern int scale(int x, int factor);

/* An old-style definition. */
int scale(int x, int factor)
{
    return x * factor;
}
