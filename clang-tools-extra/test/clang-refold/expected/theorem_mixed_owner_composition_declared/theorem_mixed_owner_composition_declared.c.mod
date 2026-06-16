// RUN: %clang-refold-tester-with-lines theorem_mixed_owner_composition_declared
// test114.c: build a typedef name and a function name from nested paste.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define TYPE(T) XCAT(T,_t)
#define FN(T) XCAT(make_,TYPE(T))

typedef long TYPE(long);
TYPE(long) global114 = 2;

int FN(long)(void) { return global114; }

int main(void) {
  return FN(long)() == 2 ? 0 : 1;
}
