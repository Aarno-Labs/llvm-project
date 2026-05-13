// RUN: %clang-refold-tester counter2
#define PRINT(fmt, ...) printf(fmt, __COUNTER__, __VA_ARGS__)

int main() {
  unsigned int x[5];
  unsigned int y[] = {10, 20, 30, 40, 50};
  unsigned int z[] = {2, 4, 6, 8, 10};
  for (size_t i = 0; i < 5; i++) {
    x[i] = y[i] + z[i];
  }

  PRINT("[%u]: %u = %u + %u\n", x[0], y[0], z[0]);
  PRINT("[%u]: %u = %u + %u\n", x[1], y[1], z[1]);
  printf("[%u]: %u = %u + %u\n", 10, x[2], y[2], z[2]);
  printf("[%u]: %u = %u + %u\n", 15, x[3], y[3], z[3]);
  printf("[%u]: %u = %u + %u\n", 4, x[4], y[4], z[4]);

  return 0;
}
