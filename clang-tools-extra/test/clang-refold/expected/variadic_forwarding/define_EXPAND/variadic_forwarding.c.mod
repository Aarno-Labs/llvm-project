// RUN: %clang-refold-tester variadic_forwarding FIRST
// RUN: %clang-refold-tester variadic_forwarding SECOND
// RUN: %clang-refold-tester variadic_forwarding THIRD
// RUN: %clang-refold-tester variadic_forwarding EXPAND
// Test: variadic_forwarding
// Refold intent: preserve variadic forwarding and nested wrapper argument structure

#define PACK2(a, b, ...) ((a) + (b) + (__VA_ARGS__))
#define WRAP(x, ...) PACK2(x, __VA_ARGS__)
int main(void) {
  int v = ((1) - (2) + (3));
  return v == (1 + 2 + 3) ? 0 : 1;
}
