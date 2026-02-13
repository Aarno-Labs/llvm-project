// RUN: %clang-refold-tester macro_call_chaining1 BODYMOD
// RUN: %clang-refold-tester macro_call_chaining1 ARGMOD
#define PICK1() INC
#define PICK2() DEC

#define INC(x) ((x)+1)
#define DEC(x) ((x)-1)

int main(void) {
  int a = PICK1()(10); // INC(10)
  int b = PICK2()(10); // DEC(10)
  return (a == 11 && b == 9) ? 0 : 1;
}
