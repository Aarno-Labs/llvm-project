// RUN: %clang-refold-tester-verify-off header_zero_token_pragma_moved_across_tu_tokens
// A header that holds only a surviving pragma, which B prints before tokens
// the translation unit spells before the `#include`.
//
// The header has no tokens, so it sits at the gap its own line prints at.  The
// unchanged `+ 2` is moved: deleted before the include and inserted after the
// header's line, inside the header, where the translation-unit anchor could
// not say which side of the directive it takes.  This used to emit A unchanged
// at verify-off.
int only_a = 1 + 2
#include "pragma_only_header.h"
;
