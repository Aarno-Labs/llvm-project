// RUN: %clang-refold-tester undef_preserve_object_like
#define FLAG 1
#undef FLAG
int value(void) {
  return FLAG;
}
