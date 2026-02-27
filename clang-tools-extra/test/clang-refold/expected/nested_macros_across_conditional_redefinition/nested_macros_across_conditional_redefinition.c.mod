// RUN: %clang-refold-tester-with-lines nested_macros_across_conditional_redefinition

// test107.c: nested macros across conditional redefinition.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define MODE_A 1
#if MODE_A
  #define MODE_NAME a
#else
  #define MODE_NAME b
#endif

#define MAKE(tag) XCAT(mode_,tag)
int mode_a(void) { return 13; }
int mode_b(void) { return 17; }

int main(void) {
  return mode_b() == 13 ? 0 : 1;
}
