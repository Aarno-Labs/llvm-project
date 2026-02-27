// RUN: %clang-refold-tester-with-lines xcat_forces_extra_expansion_over_cat

// test97.c: XCAT forces one more expansion step than CAT.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define A foo_
#define B bar

int foobar(void) { return 7; }
int foo_bar(void) { return 9; } // different symbol

int main(void) {
  // XCAT(A,B) => CAT(foo_,bar) => foobar
  return foobar() == 7 ? 0 : 1;
}
