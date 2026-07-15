// RUN: %clang-refold-tester vaopt_descendant_stringify
#define PRINT_VARS(...) __VA_OPT__( \
    PRINT_VARS_GET_MACRO(__VA_ARGS__, PRINT_4, PRINT_3, PRINT_2, PRINT_1)(__VA_ARGS__) \
)

// The routing mechanism
#define PRINT_VARS_GET_MACRO(_1, _2, _3, _4, NAME, ...) NAME

// Base expansions: Print the string name, then evaluate the variable
#define PRINT_1(a)          printf("%s = %d", #a, a);
#define PRINT_2(a, b)       PRINT_1(a) printf("; "); PRINT_1(b)
#define PRINT_3(a, b, c)    PRINT_2(a, b) printf("; "); PRINT_1(c)
#define PRINT_4(a, b, c, d) PRINT_3(a, b, c) printf("; "); PRINT_1(d)


int main() {
  int a = 1;
  int y = 20;
  int z = 30;

  // 1. Multiple variables
  PRINT_VARS(a, y, z)
  // Output: x = 10; y = 20; z = 30

  printf("\n");

  // 2. Completely empty (safe expansion, prints nothing)
  PRINT_VARS()

  return 0;
}
