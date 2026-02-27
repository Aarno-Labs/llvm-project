// RUN: %clang-refold-tester-with-lines stringify_basic

// test74: Simple stringify.
//#include <string.h>
#define S(x) #x
const char *s74 = S(OPEN);
int main(){ return (int)strlen(s74); }
