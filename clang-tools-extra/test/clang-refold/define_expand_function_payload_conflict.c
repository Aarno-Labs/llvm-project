// RUN: %clang-refold-tester define_expand_function_payload_conflict
int before = 1;
#define FOO(x) ((x)+1)
int after = 2;
#define BAR(x) ((x)*2)

int main(void) {
  return FOO(3) + BAR(4);
}
