// RUN: %clang-refold-tester include_closure_preserve_comment_gap
#define FOO(X,Y) ((X)+(Y))
int x[] = {
4
/* Non-whitespace TU text between the two include directives prevents the
   include-closure fallback from replacing this directive run structurally. */
};
