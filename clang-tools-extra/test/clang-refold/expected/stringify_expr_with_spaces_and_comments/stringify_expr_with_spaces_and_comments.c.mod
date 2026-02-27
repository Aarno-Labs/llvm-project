// RUN: %clang-refold-tester-with-lines stringify_expr_with_spaces_and_comments

// test76: stringify expression with spaces/comments.
//#include <string.h>
#define S(x) #x
const char *s76 = S(a - b);
int main(){ return (int)strlen(s76); }
