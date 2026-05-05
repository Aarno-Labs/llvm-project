// RUN: %clang-refold-tester pure_ins_plus_hdr_expansion2 XXX
int some_prefix_func(char* c);
#pragma once

#ifdef XXX
int first(short x);
#define FOO(X) (X + X)
int last(short x);
#else
float first(float x);
#define FOO(X) X
float last(float x);
#endif
int some_suffix_func();


int main() {
  return FOO(5);
}
