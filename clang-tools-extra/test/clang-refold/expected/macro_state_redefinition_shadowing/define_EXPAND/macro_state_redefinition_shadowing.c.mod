// RUN: %clang-refold-tester macro_state_redefinition_shadowing EXPAND
// RUN: %clang-refold-tester macro_state_redefinition_shadowing NOEXPAND
int main() {
  int x = 1;
  #define FOO 5
  int y1 = 5;
  int y = y1 ;
  return (x + y ) * 50;
}
