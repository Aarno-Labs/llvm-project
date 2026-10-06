// RUN: %clang-refold-tester relocated_skipped_include_guarded_by_controlling_macro
// Inlining a header to realize an edit inside it moves its directives into
// the translation unit's directory, including the ones Clang skipped because
// the header they name was already included. A skipped quoted operand that
// resolved beside the header no longer does: `rsi_leaf.h` would now fail, or
// as here find the unrelated `headers/rsi_leaf.h` through `-I headers`. The
// relocated directive is kept verbatim inside a test of the controlling macro
// Clang found defined when it skipped the header, so it is never looked up. A
// skipped operand that still selects the same header from the new location,
// `relocated_skipped_include/rsi_leaf.h` through `-I headers`, stays as
// written. Regression for brotli's enc/ units, whose inlined common/ headers
// carry `#include "platform.h"`.
#include "relocated_skipped_include/rsi_leaf.h"
#include "relocated_skipped_include/rsi_parent.h"
int main(void) { return rsi_leaf + rsi_parent; }
