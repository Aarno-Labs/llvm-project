// RUN: %clang-refold-tester include_hunk_edge_retraction_leaves_deletion_after_header_token
// Deleting a declaration that directly follows an included header must leave
// the `#include` line, and the directives between it and the declaration,
// untouched.
//
// The header ends in `;` and so does the deleted declaration, so two optimal
// alignments disagree on which `;` survives and neither is forced.  The
// core-forced plan left both in one replacement that began on the header's
// `;` and wrote that same `;` back.  Retracting its left edge out of the
// include consumes the whole B side, which the retraction used to refuse; the
// include was then given up, and its closure deleted the comment and the
// `#define` in the gap after it.  The retracted hunk is a deletion this file
// owns.  Regression for tenjin's bellard/mquickjs `mquickjs.c`
// (`js_mtag_name` after `#include "mquickjs_priv.h"`).
#include "trailing_semicolon_decl.h"

/* The tag table's size. */
#define TAG_COUNT 2

static const char *tag_name[TAG_COUNT] = { "a", "b" };

int main(void) { return 0; }
