// RUN: %clang-refold-tester pure_ins_nested_cond_boundary2 XXX YYY ZZZ
#pragma once

#ifdef YYY
char blah(int y);
#ifdef XXX
int first(int x);
#define FOO(X) (X + X)
int last(int x);
#else
float first(float x);
#define FOO(X) X
float last(float x);
#endif
float hello(float x, float y);
int some_prefix_func(char* c);
#if ZZZ
int bob(int g);
void sally(char c, double d);
#endif
int some_suffix_func();
float goodbye(float x, float y);
#endif

int main() {
  return FOO(5);
}
