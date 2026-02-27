// RUN: %clang-refold-tester-with-lines nested_indirection_type_id_wrap

// test95.c: multi-level indirection: TYPE -> ID(TYPE) -> WRAP(ID(TYPE))
#define TYPE int
#define ID(x) x
#define WRAP(x) x

WRAP(ID(TYPE)) global95 = 1;

int main(void) {
  return (int)global95;
}
