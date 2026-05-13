// RUN: %clang-refold-tester undef_preserve_object_like
#define FLAG 1
int before = 1;
#undef FLAG
int after = 2;

int value(void) {
  return FLAG;
}
