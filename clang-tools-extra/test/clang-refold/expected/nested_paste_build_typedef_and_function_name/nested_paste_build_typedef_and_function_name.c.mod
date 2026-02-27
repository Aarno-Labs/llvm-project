// RUN: %clang-refold-tester-with-lines nested_paste_build_typedef_and_function_name

// test114.c: build a typedef name and a function name from nested paste.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define TYPE(T) XCAT(T,_t)
#define FN(T) XCAT(make_,TYPE(T))

typedef long long_t;
TYPE(long) global114 = 2;

int make_long_t(void) { return global114; }

int main(void) {
  return make_long_t() == 2 ? 0 : 1;
}
