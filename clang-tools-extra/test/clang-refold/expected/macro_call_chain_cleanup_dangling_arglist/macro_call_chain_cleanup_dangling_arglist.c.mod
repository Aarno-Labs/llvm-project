// RUN: %clang-refold-tester macro_call_chain_cleanup_dangling_arglist
#define CAT(a,b) a##_##b
#define XCAT(a,b) CAT(a,b)

#define TYPE(T) XCAT(T,t)
#define FN(T) XCAT(make,TYPE(T))

typedef int TYPE(long);
TYPE(long) global114 = 2;

int FN(long)(void) { return global114; }

int main(void) {
  return FN(long)() == 2 ? 0 : 1;
}
