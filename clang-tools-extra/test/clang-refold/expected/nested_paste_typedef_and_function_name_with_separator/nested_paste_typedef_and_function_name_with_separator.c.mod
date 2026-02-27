// RUN: %clang-refold-tester-with-lines nested_paste_typedef_and_function_name_with_separator

// test114.c: build a typedef name and a function name from nested paste.
#define CAT(a,b) a##_##b
#define XCAT(a,b) CAT(a,b)

#define TYPE(T) XCAT(T,t)
#define FN(T) XCAT(make,TYPE(T))

typedef int TYPE(long);
TYPE(long) global114 = 2;

int make_long_t(void) { return global114; }

int main(void) {
  return make_long_t() == 2 ? 0 : 1;
}
