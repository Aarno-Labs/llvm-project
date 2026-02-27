// RUN: %clang-refold-tester-with-lines stringify_of_nested_expansion

// test100.c: stringify nested expansion.
#define STR1(x) #x
#define STR(x) STR1(x)

#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define NAME foo
#define SUF  bar

const char *s100 = STR(XCAT(NAME,SUF)); // "foobar"

int main(void) {
  return (s100[0] == 'f' && s100[3] == 'b') ? 0 : 1;
}
