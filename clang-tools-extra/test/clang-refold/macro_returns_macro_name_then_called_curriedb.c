// RUN: %clang-refold-tester-with-lines macro_returns_macro_name_then_called_curriedb

// test103.c: macro expands to the name of a macro, then called.
#define PICK1c() PICK1b
#define PICK1b() PICK1a
#define PICK1a() PICK1
#define PICK1() INC
#define PICK2() DEC

#define INC(x) ((x)+1)
#define DEC(x) ((x)-1)

int main(void) {
  int a = PICK1c()()()()(10); // INC(10)
  int b = PICK2()(10); // DEC(10)
  return (a == 11 && b == 9) ? 0 : 1;
}
