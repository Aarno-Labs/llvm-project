// RUN: %clang-refold-tester include_hunk_edge_splits_around_reproduced_header_tail
// A hunk that starts on an included header's trailing tokens, where the edit
// writes new tokens just before them, must split around them rather than
// straddle the include.
//
// The edit adds a parameter to both header declarations and replaces the two
// globals after the `#include` with a comment.  Keeping `on_header`'s `);`
// pays the header's depth for each of the new parameter's eight tokens, while
// deleting it pays that depth at most twice, so the owner-depth tie-break
// prefers maps that delete the `);` and match the globals' `*` and `;` to the
// ones B writes instead.  None of them is forced, so the core-forced plan left
// one hunk from the header's `)` to `main`: the header could not realize the
// tokens past its cover, the file
// had no spelling for the header's `);`, and giving the header up would have
// absorbed the edit to `on_rule`.  The whole translation unit then had no
// admissible refold.
//
// B writes the `);` intact after the new parameter, whose own `)`s are not
// followed by `;`, so the hunk splits there: the parameter is inserted inside
// the header, the `);` is kept, and the globals' replacement belongs to the
// file.  Regression for tenjin's piotrl/c_markdown `main.c` (`markdown.h`
// followed by globals moved into a struct).
#include "split_tail_decls.h"

int *first_line;
int *second_line;
int main(void) { return 0; }
