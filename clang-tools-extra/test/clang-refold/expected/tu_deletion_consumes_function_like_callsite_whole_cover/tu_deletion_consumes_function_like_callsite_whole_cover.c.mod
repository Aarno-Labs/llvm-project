// RUN: %clang-refold-tester-verify-off tu_deletion_consumes_function_like_callsite_whole_cover
// Regression: deleting whole statements whose initializers are function-like
// macro invocations.  A function-like invocation maps its body tokens to the
// macro name and its argument tokens to their spellings inside the
// parentheses, so no token spelling equals the recorded callsite extent.  The
// direct-TU span walk therefore saw the rest of each callsite, `(OP)` and
// `ID(`...`)`, as an unproved internal source gap, and the only refold left
// was the raw edited stream.  The producer's root invocation record proves
// each whole cover lies inside its callsite, so the span consumes the callsite
// once and both statements are deleted in place.
#define STR1(x) #x
#define STR(x) STR1(x)
#define ID(x) x
#define OP add

int before = 1;
int after = 2;
