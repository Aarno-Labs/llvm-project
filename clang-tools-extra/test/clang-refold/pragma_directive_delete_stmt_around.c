// RUN: %clang-refold-tester pragma_directive_delete_stmt_around
// Deleting a statement after a passed-through `#pragma` must keep the pragma
// and the statement before it.
//
// `;` and `int` each occur on both sides of the pragma, so the two anchors that
// would bound the deletion are not core-forced and are suppressed.  That widens
// the hunk into a replacement spanning the pragma, which no owner covers, and
// the attempt asks for the terminal carrier.
//
// The payload of that replacement is spelled by A tokens at the hunk's own
// edges, so re-anchoring them narrows it back to a deletion -- but there are
// three ways to do that, and they are equally optimal alignments.  They are
// separated by what they do to newlines: deleting `; int b = 2` takes the
// pragma's own line, and deleting `b = 2;\nint` fuses the two lines around it,
// so both renumber every line that follows.  Only deleting `int b = 2;` leaves
// the line count alone, and `__LINE__` and `#line` are semantic, so that is the
// one narrowing that preserves the surviving source.
//
// The blank line it leaves is the point rather than an artifact: removing the
// newline too would shift every later line.
int a = 1;
#pragma message("x")
int b = 2;
int c = 3;
