#define FOO 7
int before = 1;
#include "child_undef.h"
int after = 2;
int use = FOO;
