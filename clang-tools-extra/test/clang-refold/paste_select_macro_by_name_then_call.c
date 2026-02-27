// RUN: %clang-refold-tester-with-lines paste_select_macro_by_name_then_call

// test102.c: select a macro by pasting its name.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define OP_add(a,b) ((a)+(b))
#define OP_mul(a,b) ((a)*(b))
#define APPLY(op,a,b) XCAT(OP_,op)(a,b)

int main(void) {
  int x = APPLY(add, 2, 3);
  int y = APPLY(mul, 2, 3);
  return (x == 5 && y == 6) ? 0 : 1;
}
