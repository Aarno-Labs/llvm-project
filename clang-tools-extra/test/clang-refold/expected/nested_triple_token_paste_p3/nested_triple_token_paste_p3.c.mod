// RUN: %clang-refold-tester-with-lines nested_triple_token_paste_p3

// test98.c: triple paste with nesting.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)
#define P3(a,b,c) XCAT(XCAT(a,b),c)

#define PRE pre_
#define MID mid_
#define SUF suf

int pre_mid_sufx(void) { return 11; }

int main(void) {
  return P3(PRE, MID, sufx)() == 11 ? 0 : 1;
}
