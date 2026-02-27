// RUN: %clang-refold-tester-with-lines stringify_and_paste_same_macro

// test75: stringify + paste in same macro.
//#include <string.h>
#define BOTH(x) const char* str_##x = #x;
BOTH(open)
int main(){ return (int)strlen(str_open); }
