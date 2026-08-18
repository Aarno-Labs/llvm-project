// RUN: %clang-refold-tester pragma_operator_from_macro_arg_folds_back
// A _Pragma whose operand is a stringified macro formal (_Pragma(#x)) takes its
// content from the invocation's argument, so a content edit must fold back into
// that argument rather than materialize a raw #pragma over the invocation.
//
// The consumer cannot find the argument on its own.  Such a pragma is reported
// at the invocation's own line, so its site holds no pragma spelling, and
// matching the content against argument text would be the repeated-spelling
// provenance the refolder forbids.  The producer records which invocation and
// which argument supplied the content, and the fold rewrites exactly those
// bytes.
//
// The fold also requires the replacement content to be stringify-normalized
// already, because `#x` strips and collapses whitespace: content that would not
// reproduce itself through stringification is materialized instead.
//
// Direct _Pragma content edits fold back into the operator itself (see
// pragma_operator_message_content_folds_back).
#define DIAG(x) _Pragma(#x)
DIAG(message("bye"))
int a = 1;
