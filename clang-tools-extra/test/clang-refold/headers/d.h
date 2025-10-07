#pragma once

#ifdef XXX
int first(int x);
#define FOO(X) (X + X)
int last(int x);
#else
float first(float x);
#define FOO(X) X
float last(float x);
#endif
