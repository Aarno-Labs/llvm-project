// RUN: %clang-refold-tester include_hunk_edge_retracts_out_of_trailing_header_token
// A hunk that starts on an included header's last token and ends in the
// including file must retract its left edge out of the include.
//
// The edit deletes four declarations and writes a typedef and one retyped
// declaration in their place.  The header's closing `;` and the deleted
// declarations' `;`s are all spelled alike, so several optimal alignments
// disagree on which `;` survives, and none of them is forced.  The core-forced
// plan left all of them in one hunk that began on the header's `;`: the
// include could not realize the tokens after its cover, the file had no
// spelling for the header's token, and giving the include up spanned
// `SKIP_BLANKS`, whose body invokes `IS_BLANK`.  The whole translation unit
// then had no admissible refold.
//
// The header's `;` is the same lexeme in A and B, so handing it back to the
// untouched region keeps the edit exact and leaves a hunk the file owns: the
// `#include` line and both definitions survive.  Regression for tenjin's
// kgabis/parson `parson.c` (`<errno.h>` followed by parson's globals).
#include "trailing_semicolon_decl.h"

#define IS_BLANK(c) ((c) == ' ')
#define SKIP_BLANKS(s) while (IS_BLANK(*(s))) { (s)++; }

static int alloc_mode = 0;
static int free_mode = 0;
static int escape_slashes = 1;
static char *float_format = 0;

typedef int parson_bool_t;
int use(const char *s) { SKIP_BLANKS(s); return escape_slashes + *s; }
