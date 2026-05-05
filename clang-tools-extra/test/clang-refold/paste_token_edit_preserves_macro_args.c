// RUN: %clang-refold-tester paste_token_edit_preserves_macro_args
#define CAT(a, b) a ## b

int CAT(ab, cd) = 1;

int main(void) {
  return CAT(ab, cd);
}
