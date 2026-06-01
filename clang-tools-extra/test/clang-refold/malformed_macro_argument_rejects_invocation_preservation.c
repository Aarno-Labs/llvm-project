// RUN: %clang-refold-tester-with-lines malformed_macro_argument_rejects_invocation_preservation
// Refold intent: preserve root macros adjacent to surrounding tokens when inner edits stay structurally invertible

#define BAR(x, y) ((x) + (y))
#define FOO(x, y) BAR(2*(x), y)
int x_1 = 4;
int y_1 = 1;
int z_1 = y_1 - FOO(x_1, y_1);
int main(void) {
  return 0;
}
