// RUN: %clang-refold-tester-with-lines deferred_expansion_eval2_make_name_with_separator

// test212.c: deferred expansion without deep recursion.
#define EXPAND(x) x
#define EVAL1(x) EXPAND(x)
#define EVAL2(x) EVAL1(EVAL1(x))

#define CAT(a,b) a##_##b
#define XCAT(a,b) CAT(a,b)

#define MAKE_NAME(a,b) XCAT(a,b)
int goodbye_world(void) { return 21; }

int main(void) {
  return EVAL2(MAKE_NAME(goodbye,world))() == 21 ? 0 : 1;
}
