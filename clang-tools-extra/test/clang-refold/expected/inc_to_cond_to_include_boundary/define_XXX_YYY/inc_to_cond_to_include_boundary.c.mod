// RUN: %clang-refold-tester inc_to_cond_to_include_boundary XXX YYY
#pragma once

#ifdef YYY
char blah(int y);
int some_prefix_func(char* c);
#ifdef XXX
int first(int x);
int haha(double x);
#define BAR(X) (X * 3)
#include "a.h"
#define FOO(X) (X + X)
void cry(int x, int y);
int last(int x);
#else
float first(float x);
#define FOO(X) X
float last(float x);
#endif
int some_suffix_func();
float hello(float x, float y);
#if ZZZ
int bob(int g);
void sally(char c, double d);
#endif
float goodbye(float x, float y);
#endif

int main() {
  return FOO(5);
}
