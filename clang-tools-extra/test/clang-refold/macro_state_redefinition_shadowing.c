// RUN: %clang-refold-tester macro_state_redefinition_shadowing EXPAND
// RUN: %clang-refold-tester macro_state_redefinition_shadowing NOEXPAND
int main() {
  int x = 1;
  #define FOO 5
  int y1 = FOO;
  int y2 = FOO;
  #define FOO 50
  int z = FOO + 1;
  int y = y1 + y2;
  return (x + y + z) * FOO;
}
