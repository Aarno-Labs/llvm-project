// RUN: %clang-refold-tester-with-lines outer_macro_wraps_inner_paste_result

// test99.c: outer macro wraps inner paste output.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define DECL_FN(T) int XCAT(make_,T)(void) { return 3; }
DECL_FN(float)

int main(void) {
  return make_float () == 3 ? 0 : 1;
}
