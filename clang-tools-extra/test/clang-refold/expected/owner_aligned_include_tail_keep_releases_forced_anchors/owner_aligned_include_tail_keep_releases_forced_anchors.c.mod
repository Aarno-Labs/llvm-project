// RUN: %clang-refold-tester owner_aligned_include_tail_keep_releases_forced_anchors
// A hunk that starts on an included header's trailing tokens must keep them
// even when the matches that took B's copy of them are forced.
//
// The edit adds a parameter to both header declarations and replaces the two
// globals after the `#include` with a prototype.  Keeping `on_header`'s `);`
// pays the header's depth for each of the new parameter's four tokens, while
// deleting it pays that depth at most twice, so the core objective deletes the
// `);` and matches the globals' `*` and two `;`s to the parameter's `*`, the
// `;` B writes after it, and the prototype's `;`.  Every optimal map makes
// those matches, so they are forced: the hunk from the header's `)` to the
// first global's `*` holds no copy of the `);`, and splitting the hunk around
// one cannot help.  The header could not realize the tokens past its cover,
// giving it up would have absorbed the edit to `on_rule`, and the whole
// translation unit had no admissible refold.
//
// The `);` alone is a longest common subsequence of the region between the
// `(` before it and the prototype's `;`, so anchoring it to B's copy keeps the
// alignment maximum-length there.  The repair releases the two forced
// anchors that cross the copy, inserts the parameter inside the header, and
// leaves the prototype to replace the globals in the file.  Shape of tenjin's
// piotrl/c_markdown `main.c` when the globals' replacement is a declaration.
#ifndef SPLIT_TAIL_DECLS_H
#define SPLIT_TAIL_DECLS_H
int on_rule(struct state *s);
int on_header(struct state *s);
#endif

void reset_state(void);
int main(void) { return 0; }
