// RUN: %clang-refold-tester repeated_include
// TODO: What should the desired behavior be for repeated includes? Currently we
//       treat each include instance as a distinct region in the preprocessed
//       code, but we may want to tie all include ids that each reference the
//       same resolved path.
#include "a.h"
#include "b.h"
void hello(const char *str);
#define BAR(X) X
void world(int x, float y);

int main() {
  return BAR(2);
}
