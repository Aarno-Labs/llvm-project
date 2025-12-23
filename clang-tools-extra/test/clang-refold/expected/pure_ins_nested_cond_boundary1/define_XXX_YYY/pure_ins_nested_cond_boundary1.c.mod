// RUN: %clang-refold-tester pure_ins_nested_cond_boundary1 XXX YYY
#pragma once

#ifdef YYY
char blah(int y);
int some_prefix_func(char* c);
#ifdef XXX
int first(short  x);
#define FOO(X) (X + X)
int last(short  x);
#else
float first(float x);
#define FOO(X) X
float last(float x);
#endif
int some_suffix_func();
float hello(float x, float y);
#endif

int main() {
  return FOO(5);
}
