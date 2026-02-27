// RUN: %clang-refold-tester-with-lines nested_paste_and_stringify_combined

// test115.c: combine XCAT, P3-ish chaining, and stringify.
#define STR1(x) #x
#define STR(x) STR1(x)

#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)
#define P3(a,b,c) XCAT(XCAT(a,b),c)

#define A pre_
#define B mid_
#define C suf

const char *s115 = STR(P3(A,B,C)); // "pre_mid_suf"

int main(void) {
  return (s115[0] == 'p' && s115[4] == 'm') ? 0 : 1;
}
