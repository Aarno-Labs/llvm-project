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
#if ZZZ
int bob(int g);
void sally(char c, double d);
#endif
float goodbye(float x, float y);
#endif
