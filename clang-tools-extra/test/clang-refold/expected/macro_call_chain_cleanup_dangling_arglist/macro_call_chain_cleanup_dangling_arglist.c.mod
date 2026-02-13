// RUN: %clang-refold-tester macro_call_chain_cleanup_dangling_arglist
#define CAT(a,b) a##_##b
#define XCAT(a,b) CAT(a,b)

#define TYPE(T) XCAT(T,t)
#define FN(T) XCAT(make,TYPE(T))

typedef int long_t;
long_t global114 = 2;

int make_long_t(void) { return global114; }

int main(void) {
  return make_long_t() == 2 ? 0 : 1;
}
