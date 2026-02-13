// RUN: %clang-refold-tester macro_call_chain_preserve_suffix_call
#define EXPAND(x) x
#define EVAL1(x) EXPAND(x)
#define EVAL2(x) EVAL1(EVAL1(x))

#define CAT(a,b) a##_##b
#define XCAT(a,b) CAT(a,b)

#define MAKE_NAME(a,b) XCAT(a,b)
int hello_world(void) { return 21; }

int main(void) {
  return EVAL2(MAKE_NAME(hello,world))() == 21 ? 0 : 1;
}
