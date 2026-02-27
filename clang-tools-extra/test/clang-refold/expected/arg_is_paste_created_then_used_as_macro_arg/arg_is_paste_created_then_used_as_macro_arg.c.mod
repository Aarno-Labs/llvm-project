// RUN: %clang-refold-tester-with-lines arg_is_paste_created_then_used_as_macro_arg

// test113.c: arg itself is paste-created: USE(XCAT(pre_,T))
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define USE(x) int x(void) { return 31; } \
               int call_##x(void) { return x(); }

USE(XCAT(pre_,long))

int main(void) {
  return call_pre_long () == 31 ? 0 : 1;
}
