// RUN: env CLANG_REFOLD_TEST_ONLY_FORCE_EXPAND_INCLUDE=forced_expand_marked.h %clang-refold-tester verify_repair_expands_marked_include
// Regression: the closing output check narrows onto an include, not only onto a
// macro invocation.  Before this, a divergence no macro invocation covered had
// no owner at all and condemned the whole translation unit.
//
// An include the check rules out is expanded through the ordinary
// materialization path -- the same one an include carrying an edit takes -- so
// narrowing contributes no expansion machinery of its own.  Two properties are
// asserted by the refolded output: the marked include is inlined as header
// *source*, so a directive it owns is still a directive afterwards, and the
// unmarked include keeps its own form.  Giving up one region must not give up
// the file.
#include "forced_expand_marked.h"
#include "forced_expand_untouched.h"
int tu_value = 9;
