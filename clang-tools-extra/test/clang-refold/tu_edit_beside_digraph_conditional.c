// RUN: %clang-refold-tester tu_edit_beside_digraph_conditional
//
// Regression: an edit in a translation unit whose conditional is spelled with
// the `%:` digraph introducer.  The producer used to find conditional groups by
// scanning for `#`, so it recorded none here, while the consumer's lexer-based
// census found the group and could not bind it.  The census was incomplete and
// every edit in the file refused.  Groups now come from the preprocessor's own
// directive events.
int before = 1;
%:if 1
int inside = 2;
%:endif
int after = 3;
