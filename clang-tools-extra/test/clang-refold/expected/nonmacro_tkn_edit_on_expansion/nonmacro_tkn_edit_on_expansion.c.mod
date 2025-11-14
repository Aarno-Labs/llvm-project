// RUN: %clang-refold-tester nonmacro_tkn_edit_on_expansion
#define MAC(x) int x;
long  z;
MAC(foo)

int main() {
  return 0;
}
