// RUN: %clang-refold-tester-with-lines invocation_comments_and_quotes_in_args

// test90: comments and quotes in args.
//#include <string.h>
#define R2(x,y) const char* s90 = x; int n90 = y;
R2("(", '(')
int main(){ return (int)strlen(s90) + n90; }
