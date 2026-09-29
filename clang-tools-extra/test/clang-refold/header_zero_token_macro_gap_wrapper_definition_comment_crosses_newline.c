// RUN: %clang-refold-tester-verify-off header_zero_token_macro_gap_wrapper_definition_comment_crosses_newline
//
// Regression: a zero-token wrapper call in a rewritten header gap is proved
// neutral when its definition continues past a block comment that crosses a
// newline.  The producer used to end the directive's extent at the first
// physical newline, inside the comment; the extent then did not contain the
// definition's last token, so it was omitted along with every replacement
// token's source range, the neutrality proof could not walk the definition,
// and the header was realized from B.  The extent is now where Clang's lexer
// stands after the end-of-directive token.
#define KEEP(x) ((x) + 1)
#define EMPTY()
#define WRAP_EMPTY() /* spans
   two lines */ EMPTY()

int untouched = KEEP(5);

#include "macro_gap.h"
