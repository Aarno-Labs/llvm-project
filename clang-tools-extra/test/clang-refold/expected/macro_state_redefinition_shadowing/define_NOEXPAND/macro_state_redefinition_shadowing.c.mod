// RUN: %clang-refold-tester macro_state_redefinition_shadowing EXPAND
// RUN: %clang-refold-tester macro_state_redefinition_shadowing NOEXPAND
int main() {
  int y2 = 5;
  #define FOO 50
  int z = FOO + 1;
  int y =  y2;
  return ( y + z) * FOO;
}
